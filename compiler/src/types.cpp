#include "fb/types.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fb {
namespace {

/// What two known values give, or nullopt where they divide by 0 or pass what an Int64 holds. / and
/// % round toward 0, as an Int's do.
std::optional<std::int64_t> worked_out(const value_op_t op, const std::int64_t a,
                                       const std::int64_t b) {
    std::int64_t number = 0;
    switch (op) {
    case value_op_t::ADD:
        return __builtin_add_overflow(a, b, &number) ? std::nullopt : std::optional(number);
    case value_op_t::SUBTRACT:
        return __builtin_sub_overflow(a, b, &number) ? std::nullopt : std::optional(number);
    case value_op_t::MULTIPLY:
        return __builtin_mul_overflow(a, b, &number) ? std::nullopt : std::optional(number);
    case value_op_t::DIVIDE:
        if (b == 0 || (a == std::numeric_limits<std::int64_t>::min() && b == -1)) {
            return std::nullopt;
        }

        return a / b;
    case value_op_t::REMAINDER:
        if (b == 0) {
            return std::nullopt;
        }

        // x % -1 is 0, and Int64's minimum % -1 is undefined behavior in C++.
        return b == -1 ? 0 : a % b;
    case value_op_t::NONE:
        break;
    }

    std::unreachable();
}

/// How tightly an operation binds: + and - least, then *, /, and %, then a value or parameter.
int binding(const value_op_t op) {
    switch (op) {
    case value_op_t::ADD:
    case value_op_t::SUBTRACT:
        return 1;
    case value_op_t::MULTIPLY:
    case value_op_t::DIVIDE:
    case value_op_t::REMAINDER:
        return 2;
    case value_op_t::NONE:
        break;
    }

    return 3;
}

constexpr auto value_symbols = std::to_array<std::string_view>({ "", "+", "-", "*", "/", "%" });

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
    { .name = "CLong", .type = type_id_t::CLONG },
    { .name = "CULong", .type = type_id_t::CULONG },
    { .name = "CWChar", .type = type_id_t::CWCHAR },
    { .name = "Float32", .type = type_id_t::FLOAT32 },
    { .name = "Float64", .type = type_id_t::FLOAT64 },
    { .name = "String", .type = type_id_t::STRING },
    { .name = "CString", .type = type_id_t::CSTRING },
    { .name = "Char", .type = type_id_t::CHAR },
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

// These ranges rely on type_id_t listing the signed integers, then the unsigned ones, then C's.
bool is_integer(const type_id_t type) {
    return type >= type_id_t::INT8 && type <= type_id_t::CWCHAR;
}

bool is_signed(const type_id_t type) {
    return (type >= type_id_t::INT8 && type <= type_id_t::INT64) || type == type_id_t::CLONG ||
           type == type_id_t::CWCHAR || is_float(type);
}

bool is_c_integer(const type_id_t type) {
    return type >= type_id_t::CLONG && type <= type_id_t::CWCHAR;
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

std::span<const c_integers_t> every_c_integers() {
    static constexpr auto every = std::to_array<c_integers_t>({
        { .c_long = type_id_t::INT32, .c_ulong = type_id_t::UINT32, .c_wchar = type_id_t::UINT16 },
        { .c_long = type_id_t::INT64, .c_ulong = type_id_t::UINT64, .c_wchar = type_id_t::INT32 },
        { .c_long = type_id_t::INT64, .c_ulong = type_id_t::UINT64, .c_wchar = type_id_t::UINT32 },
        { .c_long = type_id_t::INT64, .c_ulong = type_id_t::UINT64, .c_wchar = type_id_t::UINT16 },
    });
    return every;
}

type_id_t on_target(const type_id_t type, const c_integers_t& integers) {
    switch (type) {
    case type_id_t::CLONG:
        return integers.c_long;
    case type_id_t::CULONG:
        return integers.c_ulong;
    case type_id_t::CWCHAR:
        return integers.c_wchar;
    default:
        return type;
    }
}

bool converts_losslessly(const type_id_t from, const type_id_t to) {
    // INVALID was already reported. Accepting it avoids a second error.
    if (from == to || from == type_id_t::INVALID || to == type_id_t::INVALID) {
        return true;
    }

    // C's integers convert where the target's would on every target.
    if (is_c_integer(from) || is_c_integer(to)) {
        return std::ranges::all_of(every_c_integers(), [&](const c_integers_t& integers) {
            return converts_losslessly(on_target(from, integers), on_target(to, integers));
        });
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
      m_values(other.m_values), m_thread(other.m_thread), m_scheduler(other.m_scheduler),
      m_incomplete(other.m_incomplete), m_measures(other.m_measures) {
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
        type.name += std::format("{}{}", i == 0 ? "" : ", ", listed(elements[i]));
        type.fields.push_back({ .name = std::to_string(i), .type = elements[i] });
    }

    type.name += ')';
    const type_id_t id = add(std::move(type));
    m_tuples.push_back(id);
    return id;
}

type_id_t type_table_t::function(const std::vector<type_id_t>& parameters, const type_id_t result,
                                 const bool sendable) {
    if (result == type_id_t::INVALID || std::ranges::contains(parameters, type_id_t::INVALID)) {
        return type_id_t::INVALID;
    }

    for (const type_id_t made : m_functions) {
        if (get(made).parameters == parameters && get(made).inner == result &&
            get(made).sendable == sendable) {
            return made;
        }
    }

    std::string text = sendable ? "Sendable Func(" : "Func(";
    for (std::size_t i = 0; i < parameters.size(); ++i) {
        text += std::format("{}{}", i == 0 ? "" : ", ", listed(parameters[i]));
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
        .sendable = sendable,
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
        text += std::format("{}{}", i == 0 ? "" : ", ", listed(parameters[i]));
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

type_id_t type_table_t::value(const std::int64_t number) {
    for (const type_id_t made : m_values) {
        if (get(made).number == number && get(made).operation == value_op_t::NONE) {
            return made;
        }
    }

    const type_id_t id = add({
        .kind = type_kind_t::VALUE,
        .name = std::to_string(number),
        .inner = type_id_t::INT64,
        .number = number,
    });
    m_values.push_back(id);
    return id;
}

namespace {

/// A part of a linear_t, and how many times it has it.
using part_t = std::pair<type_id_t, std::int64_t>;

} // namespace

struct type_table_t::linear_t {
    std::int64_t known = 0;
    /// Each part once, never 0 times, in the order the parts were made, so equal values list
    /// theirs alike.
    std::vector<part_t> parts;
};

namespace {

/// Whether a value's part is a Const type parameter, which works out in every instance, unlike
/// one computed with / or %, which can divide by 0, or a product, which can overflow.
bool always_works(const type_table_t& types, const type_id_t part) {
    return types.get(part).kind == type_kind_t::PARAMETER;
}

} // namespace

type_id_t type_table_t::value_expression(const value_op_t op, const type_id_t left,
                                         const type_id_t right) {
    if (left == type_id_t::INVALID || right == type_id_t::INVALID) {
        return type_id_t::INVALID;
    }

    const type_t& a = get(left);
    const type_t& b = get(right);
    if (a.kind == type_kind_t::VALUE && a.operation == value_op_t::NONE &&
        b.kind == type_kind_t::VALUE && b.operation == value_op_t::NONE) {
        if (const std::optional<std::int64_t> number = worked_out(op, a.number, b.number)) {
            return value(*number);
        }
    }

    // + and -, and * by a known number, combine, so equal values are one: N - 0 is N, N + N is
    // N * 2, and M + N is N + M. One that would drop a part that can fail, as (10 / N) * 0
    // would, or whose numbers pass what an Int64 holds, stays as it's written.
    if (const std::optional<linear_t> made = combine(op, linear(left), linear(right))) {
        return from_linear(*made);
    }

    // N / 1 is N.
    if (op == value_op_t::DIVIDE && b.kind == type_kind_t::VALUE &&
        b.operation == value_op_t::NONE && b.number == 1) {
        return left;
    }

    return computed(op, left, right);
}

type_table_t::linear_t type_table_t::linear(const type_id_t type) const {
    const type_t& info = get(type);
    if (info.kind == type_kind_t::VALUE && info.operation == value_op_t::NONE) {
        return { .known = info.number };
    }

    // What value_expression() combined is made of its parts, and the rest is a part of its own.
    if (info.kind == type_kind_t::VALUE) {
        if (const std::optional<linear_t> parts =
                combine(info.operation, linear(info.parameters[0]), linear(info.parameters[1]))) {
            return *parts;
        }
    }

    return { .parts = { { type, 1 } } };
}

std::optional<type_table_t::linear_t>
type_table_t::combine(const value_op_t op, const linear_t& left, const linear_t& right) const {
    if (op == value_op_t::ADD || op == value_op_t::SUBTRACT) {
        linear_t sum = left;
        const std::int64_t sign = op == value_op_t::ADD ? 1 : -1;
        std::int64_t known = 0;
        if (__builtin_mul_overflow(right.known, sign, &known) ||
            __builtin_add_overflow(sum.known, known, &sum.known)) {
            return std::nullopt;
        }

        for (const auto& [part, count] : right.parts) {
            std::int64_t added = 0;
            if (__builtin_mul_overflow(count, sign, &added)) {
                return std::nullopt;
            }

            const auto found = std::ranges::find(sum.parts, part, &part_t::first);
            if (found == sum.parts.end()) {
                sum.parts.emplace_back(part, added);
            } else if (__builtin_add_overflow(found->second, added, &found->second)) {
                return std::nullopt;
            } else if (found->second == 0) {
                if (!always_works(*this, part)) {
                    return std::nullopt;
                }

                sum.parts.erase(found);
            }
        }

        std::ranges::sort(sum.parts);
        return sum;
    }

    if (op != value_op_t::MULTIPLY || (!left.parts.empty() && !right.parts.empty())) {
        return std::nullopt;
    }

    linear_t product = left.parts.empty() ? right : left;
    const std::int64_t by = left.parts.empty() ? left.known : right.known;
    if (by == 0) {
        if (!std::ranges::all_of(product.parts, [this](const part_t& part) {
                return always_works(*this, part.first);
            })) {
            return std::nullopt;
        }

        return linear_t{};
    }

    if (__builtin_mul_overflow(product.known, by, &product.known)) {
        return std::nullopt;
    }

    for (part_t& part : product.parts) {
        if (__builtin_mul_overflow(part.second, by, &part.second)) {
            return std::nullopt;
        }
    }

    return product;
}

type_id_t type_table_t::from_linear(const linear_t& value) {
    // The parts it adds, then those it subtracts, then the known number: "M * 2 - N + 1".
    type_id_t made = type_id_t::INVALID;
    const auto append = [this, &made](const type_id_t part, const bool adds,
                                      const type_id_t times) {
        const type_id_t term =
            times == type_id_t::INVALID ? part : computed(value_op_t::MULTIPLY, part, times);
        if (made == type_id_t::INVALID) {
            made = adds ? term : computed(value_op_t::SUBTRACT, this->value(0), term);
        } else {
            made = computed(adds ? value_op_t::ADD : value_op_t::SUBTRACT, made, term);
        }
    };
    // A count as a number to multiply by, with the sign the part is added or subtracted with,
    // or Int64's lowest itself, which has no positive.
    const auto times = [this](const std::int64_t count) {
        constexpr std::int64_t lowest = std::numeric_limits<std::int64_t>::min();
        const std::int64_t size = count < 0 && count != lowest ? -count : count;
        return size == 1 ? type_id_t::INVALID : this->value(size);
    };
    for (const auto& [part, count] : value.parts) {
        if (count > 0) {
            append(part, true, times(count));
        }
    }

    for (const auto& [part, count] : value.parts) {
        if (count < 0) {
            append(part, count == std::numeric_limits<std::int64_t>::min(), times(count));
        }
    }

    if (made == type_id_t::INVALID) {
        return this->value(value.known);
    }

    if (value.known == 0) {
        return made;
    }

    const bool adds = value.known > 0 || value.known == std::numeric_limits<std::int64_t>::min();
    return computed(adds ? value_op_t::ADD : value_op_t::SUBTRACT, made,
                    this->value(adds ? value.known : -value.known));
}

type_id_t type_table_t::computed(const value_op_t op, const type_id_t left, const type_id_t right) {
    for (const type_id_t made : m_values) {
        if (get(made).operation == op && get(made).parameters == std::vector{ left, right }) {
            return made;
        }
    }

    // "(N + 1) * 2" and "N - (M - 1)" keep a side together where the order would change it,
    // and "N - (-1)" a negative value.
    const auto part = [this, op](const type_id_t side, const bool after) {
        const type_t& info = get(side);
        const int own = binding(info.operation);
        const bool grouped =
            own < binding(op) ||
            (after && own == binding(op) &&
             (info.operation != op || (op != value_op_t::ADD && op != value_op_t::MULTIPLY)));
        const bool negative = info.kind == type_kind_t::VALUE &&
                              info.operation == value_op_t::NONE && info.number < 0 && after;
        return grouped || negative ? std::format("({})", name(side)) : std::string(name(side));
    };
    const type_id_t id = add({
        .kind = type_kind_t::VALUE,
        .name = std::format("{} {} {}", part(left, false),
                            value_symbols[static_cast<std::size_t>(op)], part(right, true)),
        .inner = type_id_t::INT64,
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
    // field of S<T[]>, would never end, and ones whose arguments double, like S<(T, T)>, would
    // take all the memory there is before they got deep.
    constexpr std::uint32_t max_depth = 64;
    if (generic == type_id_t::INVALID || std::ranges::contains(arguments, type_id_t::INVALID) ||
        m_depth > max_depth || std::ranges::any_of(arguments, [this](const type_id_t argument) {
            return measure(argument).second > max_size;
        })) {
        return type_id_t::INVALID;
    }

    if (arguments == get(generic).parameters) {
        return generic;
    }

    for (const type_id_t made : m_instances) {
        if (get(made).generic == generic && get(made).arguments == arguments) {
            // One the limit cut short is filled again where it's asked for itself.
            if (m_depth == 0 && m_incomplete.erase(made) > 0) {
                fill(made);
            }

            return made;
        }
    }

    // "Box<T>" makes "Box<Int32>".
    const std::string_view name = get(generic).name;
    std::string text(name.substr(0, name.find('<')));
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        text += std::format("{}{}", i == 0 ? "<" : ", ", listed(arguments[i]));
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

    // A part that came out INVALID was cut short by the limit on how deep fills go.
    const bool cut = (base == type_id_t::INVALID && get(generic).base != type_id_t::INVALID) ||
                     std::ranges::contains(interfaces, type_id_t::INVALID) ||
                     std::ranges::any_of(fields, [&](const field_t& field) {
                         return field.type == type_id_t::INVALID;
                     });
    if (cut) {
        m_incomplete.insert(instance);
    }

    type_t& entry = *m_entries[std::to_underlying(instance)].type;
    entry.fields = std::move(fields);
    entry.base = base;
    entry.interfaces = std::move(interfaces);
}

std::pair<std::uint32_t, std::uint32_t> type_table_t::measure(const type_id_t type) const {
    constexpr std::uint32_t ceiling = max_size + 1;
    if (const auto found = m_measures.find(type); found != m_measures.end()) {
        return found->second;
    }

    const type_t& info = get(type);
    std::uint32_t depth = 0;
    std::uint32_t size = 1;
    const auto add = [&](const type_id_t part) {
        if (part != type_id_t::INVALID) {
            const auto [part_depth, part_size] = measure(part);
            depth = std::min(ceiling, std::max(depth, part_depth + 1));
            size = std::min(ceiling, size + part_size);
        }
    };
    std::ranges::for_each(info.arguments, add);
    std::ranges::for_each(info.throws, add);
    add(info.inner);
    if (info.kind == type_kind_t::TUPLE) {
        for (const field_t& field : info.fields) {
            add(field.type);
        }
    }

    if (info.kind == type_kind_t::FUNCTION || info.kind == type_kind_t::CALLBACK) {
        std::ranges::for_each(info.parameters, add);
    }

    return m_measures[type] = { depth, size };
}

std::optional<std::vector<type_id_t>>
type_table_t::match(const type_id_t pattern, const type_id_t actual,
                    const std::vector<type_id_t>& parameters) {
    // Each parameter takes the part of `actual` where the pattern has it, and the pattern with
    // those in place has to be `actual`, which also checks what the walk doesn't look into.
    std::vector<std::optional<type_id_t>> found(parameters.size());
    const auto walk = [&](const auto& self, const type_id_t want, const type_id_t have) -> void {
        if (want == type_id_t::INVALID || have == type_id_t::INVALID) {
            return;
        }

        if (const auto place = std::ranges::find(parameters, want); place != parameters.end()) {
            std::optional<type_id_t>& argument =
                found[static_cast<std::size_t>(place - parameters.begin())];
            if (!argument) {
                argument = have;
            }

            return;
        }

        const type_t& wanted = get(want);
        const type_t& had = get(have);
        if (wanted.kind != had.kind) {
            return;
        }

        self(self, wanted.inner, had.inner);
        if (wanted.kind == type_kind_t::FUNCTION &&
            wanted.parameters.size() == had.parameters.size()) {
            for (std::size_t i = 0; i < wanted.parameters.size(); ++i) {
                self(self, wanted.parameters[i], had.parameters[i]);
            }
        }

        if (wanted.generic != type_id_t::INVALID && wanted.generic == had.generic) {
            for (std::size_t i = 0; i < wanted.arguments.size() && i < had.arguments.size(); ++i) {
                self(self, wanted.arguments[i], had.arguments[i]);
            }
        }

        if (wanted.kind == type_kind_t::TUPLE && wanted.fields.size() == had.fields.size()) {
            for (std::size_t i = 0; i < wanted.fields.size(); ++i) {
                self(self, wanted.fields[i].type, had.fields[i].type);
            }
        }
    };
    walk(walk, pattern, actual);
    std::vector<type_id_t> arguments;
    for (const std::optional<type_id_t> argument : found) {
        if (!argument) {
            return std::nullopt;
        }

        arguments.push_back(*argument);
    }

    if (substitute(pattern, parameters, arguments) != actual) {
        return std::nullopt;
    }

    return arguments;
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
            // One worked out past the limits stays a value, which lowering reports.
            const type_t& dimension = get(dimensions[i]);
            if (dimension.kind == type_kind_t::VALUE && dimension.operation == value_op_t::NONE &&
                dimension.number >= 1 && dimension.number <= max_count) {
                sizes[i] = static_cast<std::uint32_t>(dimension.number);
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
            return task(substitute(info.inner, parameters, arguments), each(info.throws),
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

        return value_expression(info.operation,
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
        return function(each(info.parameters), substitute(info.inner, parameters, arguments),
                        info.sendable);
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
                              std::vector<enum_value_t> values, const bool flags, const bool open) {
    assert(get(type).kind == type_kind_t::ENUM);
    type_t& entry = *m_entries[std::to_underlying(type)].type;
    entry.inner = integer;
    entry.values = std::move(values);
    entry.flags = flags;
    entry.open = open;
}

std::uint64_t type_table_t::enum_zero(const type_id_t type) const {
    const type_t& entry = get(type);
    return entry.flags || entry.values.empty() ? 0 : entry.values.front().bits;
}

void type_table_t::set_shared(const type_id_t type) {
    assert(get(type).kind == type_kind_t::CLASS);
    m_entries[std::to_underlying(type)].type->shared = true;
}

void type_table_t::set_c_layout(const type_id_t type) {
    assert(get(type).kind == type_kind_t::STRUCT && get(type).parameters.empty());
    m_entries[std::to_underlying(type)].type->c_layout = true;
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

bool type_table_t::holds_fixed_array(const type_id_t type) const {
    const type_t& entry = get(type);
    switch (entry.kind) {
    case type_kind_t::FIXED_ARRAY:
        return true;
    case type_kind_t::NULLABLE:
        return holds_fixed_array(entry.inner);
    case type_kind_t::STRUCT:
    case type_kind_t::TUPLE:
        return std::ranges::any_of(
            entry.fields, [this](const field_t& field) { return holds_fixed_array(field.type); });
    default:
        return false;
    }
}

std::string type_table_t::suffixed(const type_id_t type, const bool qualified,
                                   const std::string_view main) const {
    std::string text = qualified ? qualified_name(type, main) : std::string(name(type));
    if (type == type_id_t::INVALID) {
        return text;
    }

    const type_t& info = get(type);
    if (((info.kind == type_kind_t::FUNCTION || info.kind == type_kind_t::CALLBACK) &&
         info.inner != type_id_t::NONE) ||
        (info.kind == type_kind_t::TASK && !info.throws.empty())) {
        return std::format("({})", text);
    }

    return text;
}

std::string type_table_t::listed(const type_id_t type, const bool qualified,
                                 const std::string_view main) const {
    // A Func's name ends in its result's, so one that gives such a Task goes in them too.
    type_id_t last = type;
    while (last != type_id_t::INVALID &&
           (get(last).kind == type_kind_t::FUNCTION || get(last).kind == type_kind_t::CALLBACK)) {
        last = get(last).inner;
    }

    std::string text = qualified ? qualified_name(type, main) : std::string(name(type));
    if (last != type_id_t::INVALID && get(last).kind == type_kind_t::TASK &&
        !get(last).throws.empty()) {
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
            text += std::format("{}{}", i == 0 ? "" : ", ", listed(types[i], true, main));
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
    case type_kind_t::FUNCTION: {
        const std::string_view func = type.sendable ? "Sendable Func" : "Func";
        return type.inner == type_id_t::NONE
                   ? std::format("{}({})", func, list(type.parameters))
                   : std::format("{}({}): {}", func, list(type.parameters),
                                 qualified_name(type.inner, main));
    }
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
