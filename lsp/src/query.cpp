#include "fb/lsp/query.hpp"

#include "fb/ast.hpp"
#include "fb/const_value.hpp"
#include "fb/lsp/docs.hpp"
#include "fb/lsp/protocol.hpp"
#include "fb/sema.hpp"
#include "fb/types.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace fb::lsp {
namespace {

/// Whether `c` can be part of a name.
bool is_name_char(const char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           static_cast<unsigned char>(c) >= 0x80;
}

} // namespace

std::uint32_t declaration_begin(const stmt_t& statement) {
    const std::optional<std::uint32_t> start = std::visit(
        [](const auto& declaration) -> std::optional<std::uint32_t> {
            if constexpr (requires { declaration.modifiers.start; }) {
                return declaration.modifiers.start;
            } else {
                return std::nullopt;
            }
        },
        statement.node);
    return std::min(statement.span.begin, start.value_or(statement.span.begin));
}

query_t::query_t(const compilation_t& compilation, const file_id_t file)
    : m_compilation(compilation), m_file(file) {
    const analysis_t& analysis = compilation.analysis;
    for (std::size_t i = 0; i < analysis.symbols.size(); ++i) {
        const symbol_t& symbol = analysis.symbols[i];
        if (symbol.kind == symbol_kind_t::TYPE) {
            m_types.try_emplace(symbol.type, static_cast<symbol_id_t>(i));
        }
    }
}

std::optional<symbol_id_t> query_t::type_symbol(const type_id_t type) const {
    if (type == type_id_t::INVALID || std::to_underlying(type) >= analysis().types.size()) {
        return std::nullopt;
    }

    const type_id_t generic = analysis().types.get(type).generic;
    const auto found = m_types.find(generic != type_id_t::INVALID ? generic : type);
    return found != m_types.end() ? std::optional(found->second) : std::nullopt;
}

std::optional<named_t> query_t::name_of(const symbol_use_t& use) const {
    symbol_id_t id = use.symbol;
    const symbol_t* symbol = &analysis().symbol(id);
    const std::string_view text = m_compilation.sources.get(use.span.file).slice(use.span);
    // "Point(1, 2)" names the Struct, and "Super.New()" the New.
    if (symbol->kind == symbol_kind_t::CONSTRUCTOR && !text.ends_with("New")) {
        const std::optional<symbol_id_t> type = type_symbol(symbol->owner);
        if (!type) {
            return std::nullopt;
        }

        id = *type;
        symbol = &analysis().symbol(id);
    }

    const std::string_view name = symbol->name;
    if (name.empty()) {
        return std::nullopt;
    }

    // The name, or one after a qualifier or an object, "game.score", or one with type arguments
    // after it, "Box<Int>".
    std::size_t at = 0;
    if (text != name) {
        if (text.ends_with(name) && !is_name_char(text[text.size() - name.size() - 1])) {
            at = text.size() - name.size();
        } else if (!text.starts_with(name) || is_name_char(text[name.size()])) {
            return std::nullopt;
        }
    }

    const auto begin = static_cast<std::uint32_t>(use.span.begin + at);
    return named_t{
        .span = { .file = use.span.file,
                  .begin = begin,
                  .end = begin + static_cast<std::uint32_t>(name.size()), },
        .symbol = id,
    };
}

std::vector<named_t> query_t::names(const std::optional<file_id_t> file) const {
    const file_id_t in = file.value_or(m_file);
    std::vector<named_t> found;
    for (const symbol_use_t& use : analysis().uses) {
        if (use.span.file == in) {
            if (const std::optional<named_t> named = name_of(use)) {
                found.push_back(*named);
            }
        }
    }

    for (std::size_t i = 0; i < analysis().symbols.size(); ++i) {
        const symbol_t& symbol = analysis().symbols[i];
        if (symbol.span.file != in || symbol.span.empty() ||
            symbol.kind == symbol_kind_t::BUILTIN_FUNCTION ||
            (symbol.kind == symbol_kind_t::CONSTRUCTOR && !symbol.declaration)) {
            continue;
        }

        // A Property's Get and Set are Methods named where it is.
        if (symbol.kind == symbol_kind_t::METHOD &&
            m_compilation.sources.get(in).slice(symbol.span) != symbol.name) {
            continue;
        }

        found.push_back(
            { .span = symbol.span, .symbol = static_cast<symbol_id_t>(i), .declaration = true });
    }

    // One name each, its declaration's first, and of a generic one's copies, the first.
    std::ranges::sort(found, [](const named_t& a, const named_t& b) {
        if (a.span.begin != b.span.begin) {
            return a.span.begin < b.span.begin;
        }

        if (a.declaration != b.declaration) {
            return a.declaration;
        }

        return std::to_underlying(a.symbol) < std::to_underlying(b.symbol);
    });
    const auto [first, last] = std::ranges::unique(
        found, [](const named_t& a, const named_t& b) { return a.span == b.span; });
    found.erase(first, last);
    return found;
}

std::optional<named_t> query_t::name_at(const std::uint32_t offset) const {
    std::optional<named_t> best;
    for (const named_t& named : names()) {
        if (named.span.contains(offset) || named.span.end == offset) {
            // Inside a name beats being at the end of one before it.
            if (!best || named.span.contains(offset)) {
                best = named;
            }
        }
    }

    return best;
}

symbol_key_t query_t::key(const symbol_id_t id) const {
    const symbol_t& symbol = analysis().symbol(id);
    if (symbol.span.empty()) {
        return { .name = std::string(symbol.name) };
    }

    const std::filesystem::path path = m_compilation.disk_path(symbol.span.file);
    return {
        .file = path.empty() ? m_compilation.sources.get(symbol.span.file).path() : file_key(path),
        .begin = symbol.span.begin,
        .name = std::string(symbol.name),
    };
}

std::vector<symbol_id_t> query_t::symbols_of(const symbol_key_t& key) const {
    std::vector<symbol_id_t> found;
    // The name and the place first, which are cheap, and the file, whose key isn't.
    for (std::size_t i = 0; i < analysis().symbols.size(); ++i) {
        const symbol_t& symbol = analysis().symbols[i];
        if (symbol.name == key.name && symbol.span.begin == key.begin &&
            symbol.span.empty() == key.file.empty() &&
            this->key(static_cast<symbol_id_t>(i)) == key) {
            found.push_back(static_cast<symbol_id_t>(i));
        }
    }

    return found;
}

std::string query_t::type_name(const type_id_t type) const {
    if (type == type_id_t::INVALID) {
        return "?";
    }

    return std::string(analysis().types.name(type));
}

std::string query_t::signature(const symbol_id_t id) const {
    const symbol_t& symbol = analysis().symbol(id);
    const std::string owner =
        symbol.owner != type_id_t::INVALID ? type_name(symbol.owner) + "." : std::string();
    const auto parameters = [&] {
        std::string text = "(";
        for (std::size_t i = 0; i < symbol.parameters.size(); ++i) {
            const symbol_t& parameter = analysis().symbol(symbol.parameters[i]);
            text += i > 0 ? ", " : "";
            text += parameter.by_ref ? "ByRef " : "";
            text += std::string(parameter.name) + ": ";
            if (parameter.variadic) {
                const type_t& array = analysis().types.get(parameter.type);
                text +=
                    type_name(array.inner != type_id_t::INVALID ? array.inner : parameter.type) +
                    "...";
            } else {
                text += type_name(parameter.type);
            }

            if (parameter.has_default && parameter.value) {
                text += " = " + to_text(*parameter.value);
            } else if (parameter.has_default) {
                text += " = ...";
            }
        }

        return text + ")";
    };
    const auto type_parameters = [&] {
        std::string text;
        for (const type_id_t parameter : symbol.type_parameters) {
            text += (text.empty() ? "<" : ", ") + type_name(parameter);
        }

        return text.empty() ? text : text + ">";
    };
    const auto throws = [&] {
        std::string text;
        for (const type_id_t error : symbol.throws) {
            text += (text.empty() ? " Throws " : ", ") + type_name(error);
        }

        return text;
    };
    const auto typed = [&](const std::string& head) {
        return head + std::string(symbol.name) + ": " + type_name(symbol.type);
    };

    switch (symbol.kind) {
    case symbol_kind_t::FUNCTION:
    case symbol_kind_t::BUILTIN_FUNCTION:
    case symbol_kind_t::METHOD: {
        std::string head = symbol.modifiers.is_async ? "Async " : "";
        head += symbol.kind == symbol_kind_t::METHOD ? "Method " : "Function ";
        if (symbol.kind == symbol_kind_t::METHOD && symbol.modifiers.is_static) {
            head = "Static " + head;
        }

        std::string text =
            head + owner + std::string(symbol.name) + type_parameters() + parameters();
        if (symbol.type != type_id_t::NONE) {
            text += ": " + type_name(symbol.type);
        }

        return text + throws();
    }
    case symbol_kind_t::CONSTRUCTOR:
        return "New " + type_name(symbol.owner) + parameters() + throws();
    case symbol_kind_t::GLOBAL:
        if (symbol.owner != type_id_t::INVALID) {
            return typed("Static Field " + owner);
        }

        return typed(symbol.modifiers.is_thread_local ? "ThreadLocal Global " : "Global ");
    case symbol_kind_t::CONSTANT: {
        std::string text = typed("Const ");
        if (symbol.value) {
            text += " = " + to_text(*symbol.value);
        }

        return text;
    }
    case symbol_kind_t::LOCAL:
    case symbol_kind_t::LOOP_VARIABLE:
        return typed("Local ");
    case symbol_kind_t::PARAMETER:
        return typed(symbol.by_ref ? "ByRef " : "");
    case symbol_kind_t::FIELD:
        return typed(symbol.modifiers.is_read_only ? "ReadOnly Field " + owner : "Field " + owner);
    case symbol_kind_t::PROPERTY:
        return typed("Property " + owner);
    case symbol_kind_t::MEMBER_VARIABLE:
        return "For " + std::string(symbol.name);
    case symbol_kind_t::ALIAS:
        return "Alias " + std::string(symbol.name) + " = " + type_name(symbol.type);
    case symbol_kind_t::MODULE: {
        std::string text = "Import ";
        for (std::size_t i = 0; i < symbol.modules.size(); ++i) {
            text += (i > 0 ? ", " : "") + ast().modules().get(symbol.modules[i]).name;
        }

        return text;
    }
    case symbol_kind_t::TYPE: {
        const type_t& type = analysis().types.get(symbol.type);
        std::string text;
        switch (type.kind) {
        case type_kind_t::CLASS:
            text = "Class " + type_name(symbol.type);
            if (type.base != type_id_t::INVALID) {
                text += " Extends " + type_name(type.base);
            }

            break;
        case type_kind_t::INTERFACE:
            text = "Interface " + type_name(symbol.type);
            break;
        case type_kind_t::ENUM:
            text = "Enum " + type_name(symbol.type);
            if (type.inner != type_id_t::INVALID && type.inner != type_id_t::INT32) {
                text += ": " + type_name(type.inner);
            }

            break;
        default:
            text = "Struct " + type_name(symbol.type);
            break;
        }

        for (std::size_t i = 0; i < type.interfaces.size(); ++i) {
            if (type.kind == type_kind_t::INTERFACE) {
                text += (i == 0 ? " Extends " : ", ") + type_name(type.interfaces[i]);
            } else {
                text += (i == 0 ? " Implements " : ", ") + type_name(type.interfaces[i]);
            }
        }

        return text;
    }
    }

    return std::string(symbol.name);
}

std::string query_t::docs(const symbol_id_t id) const {
    const symbol_t& symbol = analysis().symbol(id);
    if (symbol.span.empty()) {
        return {};
    }

    std::uint32_t begin = symbol.span.begin;
    file_id_t file = symbol.span.file;
    if (symbol.declaration) {
        const stmt_t& statement = ast().get(*symbol.declaration);
        file = statement.span.file;
        begin = declaration_begin(statement);
    } else if (symbol.kind != symbol_kind_t::FIELD && symbol.kind != symbol_kind_t::TYPE) {
        return {};
    }

    return doc_comment(m_compilation.sources.get(file), begin);
}

std::optional<std::filesystem::path> query_t::declared_in(const symbol_id_t id) const {
    const symbol_t& symbol = analysis().symbol(id);
    if (symbol.span.empty() && symbol.kind == symbol_kind_t::BUILTIN_FUNCTION) {
        return std::nullopt;
    }

    std::filesystem::path path = m_compilation.disk_path(symbol.span.file);
    return path.empty() ? std::nullopt : std::optional(std::move(path));
}

std::optional<expr_id_t> query_t::expr_at(const std::uint32_t offset) const {
    std::optional<expr_id_t> best;
    std::uint32_t length = UINT32_MAX;
    for (std::size_t i = 0; i < ast().expr_count(); ++i) {
        const span_t span = ast().get(static_cast<expr_id_t>(i)).span;
        if (span.file == m_file && span.begin <= offset && offset <= span.end &&
            span.length() < length) {
            best = static_cast<expr_id_t>(i);
            length = span.length();
        }
    }

    return best;
}

std::optional<module_id_t> query_t::module() const {
    return ast().modules().of(m_file);
}

} // namespace fb::lsp
