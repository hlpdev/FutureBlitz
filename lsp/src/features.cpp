#include "fb/lsp/features.hpp"

#include "fb/ast.hpp"
#include "fb/diagnostic.hpp"
#include "fb/driver.hpp"
#include "fb/format.hpp"
#include "fb/lexer.hpp"
#include "fb/lsp/protocol.hpp"
#include "fb/lsp/query.hpp"
#include "fb/parser.hpp"
#include "fb/sema.hpp"
#include "fb/source.hpp"
#include "fb/token.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace fb::lsp {
namespace {

/// The LSP's SymbolKind numbers.
enum class symbol_kind_lsp_t : std::uint8_t {
    MODULE = 2,
    NAMESPACE = 3,
    CLASS = 5,
    METHOD = 6,
    PROPERTY = 7,
    FIELD = 8,
    CONSTRUCTOR = 9,
    ENUM = 10,
    INTERFACE = 11,
    FUNCTION = 12,
    VARIABLE = 13,
    CONSTANT = 14,
    ENUM_MEMBER = 22,
    STRUCT = 23,
    OPERATOR = 25,
    TYPE_PARAMETER = 26,
};

/// One declaration of a file, with the ones inside it.
struct outline_t {
    std::string name;
    std::string detail;
    symbol_kind_lsp_t kind = symbol_kind_lsp_t::VARIABLE;
    span_t span;
    span_t selection;
    std::vector<outline_t> children;
};

/// The text of `file` from `begin` to `end`, on one line, its runs of spaces one space.
std::string one_line(const source_file_t& file, const std::uint32_t begin,
                     const std::uint32_t end) {
    const std::string_view text = file.slice({ .file = file.id(), .begin = begin, .end = end });
    std::string line;
    for (const char c : text) {
        const bool space = c == ' ' || c == '\t' || c == '\r' || c == '\n';
        if (space && (line.empty() || line.back() == ' ')) {
            continue;
        }

        line += space ? ' ' : c;
    }

    while (line.ends_with(' ')) {
        line.pop_back();
    }

    return line;
}

/// The declarations among `block`'s statements, in order.
std::vector<outline_t> outline_of(const ast_t& ast, const source_file_t& file,
                                  const block_t& block);

/// One statement's declaration, if it is one, added to `out`.
void outline_statement(const ast_t& ast, const source_file_t& file, const stmt_id_t id,
                       std::vector<outline_t>& out) {
    const stmt_t& statement = ast.get(id);
    // With its attributes and modifiers.
    const std::uint32_t begin = declaration_begin(statement);
    const span_t whole{ .file = statement.span.file, .begin = begin, .end = statement.span.end };
    const auto add = [&](const name_t& name, std::string label, const symbol_kind_lsp_t kind,
                         std::string detail, std::vector<outline_t> children = {}) {
        if (name.text.empty() && label.empty()) {
            return;
        }

        out.push_back({
            .name = label.empty() ? std::string(name.text) : std::move(label),
            .detail = std::move(detail),
            .kind = kind,
            .span = whole,
            .selection = name.span.empty()
                             ? span_t{ .file = statement.span.file, .begin = begin, .end = begin }
                             : name.span,
            .children = std::move(children),
        });
    };

    if (const auto* function = std::get_if<function_stmt_t>(&statement.node)) {
        const std::string header = one_line(file, begin, std::max(function->header_end, begin));
        switch (function->kind) {
        case function_kind_t::FUNCTION:
            add(function->name, {}, symbol_kind_lsp_t::FUNCTION, header);
            break;
        case function_kind_t::METHOD:
        case function_kind_t::BYREF_METHOD:
            add(function->name, {}, symbol_kind_lsp_t::METHOD, header);
            break;
        case function_kind_t::CONSTRUCTOR:
            add(function->name, "New", symbol_kind_lsp_t::CONSTRUCTOR, header);
            break;
        case function_kind_t::OPERATOR:
            add(function->name, "Operator " + std::string(function->name.text),
                symbol_kind_lsp_t::OPERATOR, header);
            break;
        }
    } else if (const auto* type = std::get_if<type_stmt_t>(&statement.node)) {
        symbol_kind_lsp_t kind =
            type->is_class ? symbol_kind_lsp_t::CLASS : symbol_kind_lsp_t::STRUCT;
        if (type->is_interface) {
            kind = symbol_kind_lsp_t::INTERFACE;
        }

        add(type->name, {}, kind, one_line(file, begin, std::max(type->header_end, begin)),
            outline_of(ast, file, type->members));
    } else if (const auto* enumeration = std::get_if<enum_stmt_t>(&statement.node)) {
        std::vector<outline_t> members;
        for (const enum_member_t& member : enumeration->members) {
            if (!member.name.text.empty()) {
                members.push_back({
                    .name = std::string(member.name.text),
                    .kind = symbol_kind_lsp_t::ENUM_MEMBER,
                    .span = member.name.span,
                    .selection = member.name.span,
                });
            }
        }

        add(enumeration->name, {}, symbol_kind_lsp_t::ENUM, {}, std::move(members));
    } else if (const auto* variable = std::get_if<variable_stmt_t>(&statement.node)) {
        const std::string written =
            variable->type ? std::string(file.slice(ast.get(*variable->type).span)) : std::string();
        switch (variable->kind) {
        case variable_kind_t::CONSTANT:
            add(variable->name, {}, symbol_kind_lsp_t::CONSTANT, written);
            break;
        case variable_kind_t::FIELD:
            add(variable->name, {}, symbol_kind_lsp_t::FIELD, written);
            break;
        case variable_kind_t::GLOBAL:
        case variable_kind_t::LOCAL:
            add(variable->name, {}, symbol_kind_lsp_t::VARIABLE, written);
            break;
        }
    } else if (const auto* property = std::get_if<property_stmt_t>(&statement.node)) {
        add(property->name, {}, symbol_kind_lsp_t::PROPERTY,
            std::string(file.slice(ast.get(property->type).span)));
    } else if (const auto* extend = std::get_if<extend_stmt_t>(&statement.node)) {
        const span_t extended = ast.get(extend->type).span;
        add({ .span = extended }, "Extend " + std::string(file.slice(extended)),
            symbol_kind_lsp_t::CLASS, {}, outline_of(ast, file, extend->members));
    } else if (const auto* alias = std::get_if<alias_stmt_t>(&statement.node)) {
        add(alias->name, {}, symbol_kind_lsp_t::TYPE_PARAMETER,
            std::string(file.slice(ast.get(alias->type).span)));
    } else if (const auto* external = std::get_if<extern_stmt_t>(&statement.node)) {
        // Its Functions are the file's own.
        for (const stmt_id_t each : external->functions) {
            outline_statement(ast, file, each, out);
        }
    }
}

std::vector<outline_t> outline_of(const ast_t& ast, const source_file_t& file,
                                  const block_t& block) {
    std::vector<outline_t> out;
    for (const stmt_id_t id : block) {
        outline_statement(ast, file, id, out);
    }

    return out;
}

/// The declarations of a file, parsed on its own.
std::vector<outline_t> outline(const source_file_t& file) {
    diagnostic_bag_t ignored;
    const std::vector<std::string_view> defined = defined_symbols(build_mode_t::DEBUG);
    const std::vector<token_t> tokens = lex(file, ignored, defined);
    const ast_t ast = parse(file, tokens, ignored);
    return outline_of(ast, file, ast.items());
}

json_t document_symbol(const outline_t& symbol, const source_file_t& file,
                       const positions_t& positions) {
    json_t children = json_t::array();
    for (const outline_t& child : symbol.children) {
        children.push_back(document_symbol(child, file, positions));
    }

    json_t out{
        { "name", symbol.name },
        { "kind", static_cast<int>(symbol.kind) },
        { "range", positions.range(file, symbol.span) },
        { "selectionRange", positions.range(file, symbol.selection) },
        { "children", std::move(children) },
    };
    if (!symbol.detail.empty()) {
        out["detail"] = symbol.detail;
    }

    return out;
}

void collect_symbols(const std::vector<outline_t>& symbols, const std::string& container,
                     const source_file_t& file, const std::string& uri, const std::string& query,
                     const positions_t& positions, json_t& out) {
    for (const outline_t& symbol : symbols) {
        if (fuzzy_match(symbol.name, query)) {
            json_t information{
                { "name", symbol.name },
                { "kind", static_cast<int>(symbol.kind) },
                {
                    "location",
                    { { "uri", uri }, { "range", positions.range(file, symbol.selection) } },
                },
            };
            if (!container.empty()) {
                information["containerName"] = container;
            }

            out.push_back(std::move(information));
        }

        collect_symbols(symbol.children, symbol.name, file, uri, query, positions, out);
    }
}

/// A Location in a compilation's file, or nullopt for one that isn't on disk.
std::optional<json_t> location_of(const compilation_t& compilation, const span_t span,
                                  const positions_t& positions) {
    const std::filesystem::path path = compilation.disk_path(span.file);
    if (path.empty()) {
        return std::nullopt;
    }

    return json_t{
        { "uri", uri_of(path) },
        { "range", positions.range(compilation.sources.get(span.file), span) },
    };
}

/// A SignatureInformation for a function: its declaration, with where each parameter is in it,
/// in the client's units, and its docs.
json_t signature_information(const query_t& query, const symbol_id_t function,
                             const positions_t& positions) {
    const std::string label = query.signature(function);
    json_t parameters = json_t::array();
    std::size_t from = label.find('(') + 1;
    for (const symbol_id_t parameter : query.analysis().symbol(function).parameters) {
        const std::string_view name = query.analysis().symbol(parameter).name;
        const std::size_t begin = label.find(name, from);
        if (begin == std::string::npos) {
            break;
        }

        std::size_t end = label.find_first_of(",)", begin);
        end = end == std::string::npos ? label.size() : end;
        const std::uint32_t first = positions.length(std::string_view(label).substr(0, begin));
        const std::uint32_t last =
            first + positions.length(std::string_view(label).substr(begin, end - begin));
        parameters.push_back({ { "label", { first, last } } });
        from = end;
    }

    json_t signature{ { "label", label }, { "parameters", std::move(parameters) } };
    if (const std::string docs = query.docs(function); !docs.empty()) {
        signature["documentation"] = { { "kind", "markdown" }, { "value", docs } };
    }

    return signature;
}

/// What a call can be calling, which is typed while its arguments are: the function it resolved
/// to, and the others its callee's name has there, a Function's overloads, the Methods of the
/// name its object has, or a type's News.
std::vector<symbol_id_t> overloads(const query_t& query, const call_expr_t& call,
                                   const std::optional<symbol_id_t> resolved) {
    const analysis_t& analysis = query.analysis();
    const ast_t& ast = query.ast();
    const auto callable = [&](const symbol_id_t id) {
        const symbol_kind_t kind = analysis.symbol(id).kind;
        return kind == symbol_kind_t::FUNCTION || kind == symbol_kind_t::BUILTIN_FUNCTION ||
               kind == symbol_kind_t::METHOD || kind == symbol_kind_t::CONSTRUCTOR;
    };
    std::vector<symbol_id_t> found;
    if (resolved && callable(*resolved)) {
        found.push_back(*resolved);
    }

    const auto add = [&](const symbol_id_t id) {
        if (!std::ranges::contains(found, id)) {
            found.push_back(id);
        }
    };
    const auto news = [&](const symbol_id_t type) {
        for (const symbol_id_t member : analysis.symbol(type).members) {
            if (analysis.symbol(member).kind == symbol_kind_t::CONSTRUCTOR) {
                add(member);
            }
        }
    };

    // The files whose top-level names the call sees: its own, and its Imports'.
    std::set<file_id_t> files{ query.file() };
    if (const std::optional<module_id_t> module = query.module()) {
        for (const module_import_t& imported : ast.modules().get(*module).imports) {
            for (const module_id_t each : imported.modules) {
                files.insert(ast.modules().get(each).file);
            }
        }
    }

    // The types whose Methods an object of `type` has: it, its generic one, and its bases.
    const auto owners = [&](type_id_t type) {
        std::set<type_id_t> out;
        while (type != type_id_t::INVALID && std::to_underlying(type) < analysis.types.size() &&
               out.insert(type).second) {
            const type_t& entry = analysis.types.get(type);
            if (entry.kind == type_kind_t::NULLABLE) {
                type = entry.inner;
                continue;
            }

            if (entry.generic != type_id_t::INVALID) {
                out.insert(entry.generic);
            }

            type = entry.base;
        }

        return out;
    };

    std::string_view name;
    std::optional<std::set<type_id_t>> object;
    if (const auto* named = std::get_if<name_expr_t>(&ast.get(call.callee).node)) {
        name = named->name;
    } else if (const auto* member = std::get_if<member_expr_t>(&ast.get(call.callee).node)) {
        name = member->member.text;
        object = owners(analysis.expr(member->object).type);
    }

    if (resolved && analysis.symbol(*resolved).kind == symbol_kind_t::TYPE) {
        news(*resolved);
        return found;
    }

    for (std::size_t i = 0; i < analysis.symbols.size() && !name.empty(); ++i) {
        const auto id = static_cast<symbol_id_t>(i);
        const symbol_t& symbol = analysis.symbols[i];
        if (symbol.name != name) {
            continue;
        }

        if (object) {
            if (symbol.kind == symbol_kind_t::METHOD && object->contains(symbol.owner)) {
                add(id);
            }
        } else if (symbol.kind == symbol_kind_t::BUILTIN_FUNCTION ||
                   (symbol.kind == symbol_kind_t::FUNCTION && symbol.owner == type_id_t::INVALID &&
                    files.contains(symbol.span.file))) {
            add(id);
        } else if (symbol.kind == symbol_kind_t::TYPE && files.contains(symbol.span.file)) {
            news(id);
        }
    }

    std::erase_if(found, [&](const symbol_id_t id) { return !callable(id); });
    return found;
}

/// Markdown's code block of FutureBlitz code.
std::string code_block(const std::string& code) {
    return "```fb\n" + code + "\n```";
}

} // namespace

bool fuzzy_match(const std::string_view name, const std::string_view query) {
    const auto lower = [](const char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    };
    std::size_t at = 0;
    for (const char c : name) {
        if (at < query.size() && lower(c) == lower(query[at])) {
            ++at;
        }
    }

    return at == query.size();
}

json_t diagnostic_json(const compilation_t& compilation, const diagnostic_t& diagnostic,
                       const positions_t& positions) {
    std::string message = diagnostic.message;
    for (const std::string& note : diagnostic.notes) {
        message += "\n" + note;
    }

    // The LSP's DiagnosticSeverity: 1 for an error, 2 for a warning, and 3 for information.
    int severity = 3;
    if (diagnostic.severity == severity_t::ERROR) {
        severity = 1;
    } else if (diagnostic.severity == severity_t::WARNING) {
        severity = 2;
    }

    json_t out{
        {
            "range",
            positions.range(compilation.sources.get(diagnostic.span.file), diagnostic.span),
        },
        { "severity", severity },
        { "source", "fb" },
        { "message", message },
    };
    if (!diagnostic.code.empty()) {
        out["code"] = diagnostic.code;
    }

    json_t related = json_t::array();
    for (const label_t& label : diagnostic.labels) {
        if (std::optional<json_t> location = location_of(compilation, label.span, positions)) {
            related.push_back(
                { { "location", std::move(*location) }, { "message", label.message } });
        }
    }

    if (!related.empty()) {
        out["relatedInformation"] = std::move(related);
    }

    return out;
}

json_t hover(const query_t& query, const std::uint32_t offset, const positions_t& positions) {
    if (const std::optional<named_t> named = query.name_at(offset)) {
        std::string value = code_block(query.signature(named->symbol));
        if (const std::string docs = query.docs(named->symbol); !docs.empty()) {
            value += "\n\n" + docs;
        }

        return {
            { "contents", { { "kind", "markdown" }, { "value", value } } },
            { "range", positions.range(query.source(), named->span) },
        };
    }

    // An expression's type, where no name is.
    if (const std::optional<expr_id_t> expr = query.expr_at(offset)) {
        const expr_info_t& info = query.analysis().expr(*expr);
        if (info.type != type_id_t::INVALID && info.type != type_id_t::NONE) {
            return {
                {
                    "contents",
                    { { "kind", "markdown" }, { "value", code_block(query.type_name(info.type)) } },
                },
                { "range", positions.range(query.source(), query.ast().get(*expr).span) },
            };
        }
    }

    return nullptr;
}

json_t definition(const query_t& query, const std::uint32_t offset, const positions_t& positions) {
    // An Import's module is its file.
    if (const std::optional<module_id_t> module = query.module()) {
        for (const module_import_t& imported : query.ast().modules().get(*module).imports) {
            const auto* node =
                std::get_if<import_stmt_t>(&query.ast().get(imported.statement).node);
            if (node == nullptr || offset < node->target_span.begin ||
                offset > node->target_span.end || imported.modules.empty()) {
                continue;
            }

            const file_id_t file = query.ast().modules().get(imported.modules.front()).file;
            if (std::optional<json_t> location =
                    location_of(query.compilation(), { .file = file }, positions)) {
                return *location;
            }
        }
    }

    const std::optional<named_t> named = query.name_at(offset);
    if (!named) {
        return nullptr;
    }

    // A module's qualifier, or an Import's As name, is its module's file.
    const symbol_t& symbol = query.analysis().symbol(named->symbol);
    if (symbol.kind == symbol_kind_t::MODULE && !symbol.modules.empty()) {
        const file_id_t file = query.ast().modules().get(symbol.modules.front()).file;
        if (std::optional<json_t> location =
                location_of(query.compilation(), { .file = file }, positions)) {
            return *location;
        }
    }

    if (symbol.span.empty()) {
        return nullptr;
    }

    return location_of(query.compilation(), symbol.span, positions).value_or(nullptr);
}

json_t references(const std::span<const std::shared_ptr<const compilation_t>> compilations,
                  const std::span<const symbol_key_t> keys, const bool declaration,
                  const std::function<bool(const std::filesystem::path&)>& keep,
                  const positions_t& positions) {
    json_t out = json_t::array();
    std::set<std::pair<std::string, std::uint32_t>> seen;
    for (const std::shared_ptr<const compilation_t>& compilation : compilations) {
        const query_t query(*compilation, compilation->file);
        std::set<symbol_id_t> symbols;
        for (const symbol_key_t& key : keys) {
            const std::vector<symbol_id_t> found = query.symbols_of(key);
            symbols.insert(found.begin(), found.end());
        }

        if (symbols.empty()) {
            continue;
        }

        // Which files are kept, by file id, once each.
        std::map<file_id_t, bool> kept;
        const auto add = [&](const span_t span) {
            auto [found, added] = kept.try_emplace(span.file, false);
            if (added) {
                const std::filesystem::path path = compilation->disk_path(span.file);
                found->second = !path.empty() && keep(path);
            }

            if (!found->second ||
                !seen.emplace(file_key(compilation->disk_path(span.file)), span.begin).second) {
                return;
            }

            if (std::optional<json_t> location = location_of(*compilation, span, positions)) {
                out.push_back(std::move(*location));
            }
        };

        if (declaration) {
            for (const symbol_id_t id : symbols) {
                add(compilation->analysis.symbol(id).span);
            }
        }

        for (const symbol_use_t& use : compilation->analysis.uses) {
            if (symbols.contains(use.symbol) ||
                compilation->analysis.symbol(use.symbol).kind == symbol_kind_t::CONSTRUCTOR) {
                const std::optional<named_t> named = query.name_of(use);
                if (named && symbols.contains(named->symbol)) {
                    add(named->span);
                }
            }
        }
    }

    return out;
}

rename_set_t rename_set(const std::span<const std::shared_ptr<const compilation_t>> compilations,
                        const symbol_key_t& key) {
    rename_set_t out{ .keys = { key } };
    for (const std::shared_ptr<const compilation_t>& compilation : compilations) {
        const query_t query(*compilation, compilation->file);
        const analysis_t& analysis = compilation->analysis;
        // Each member and those it's tied to: what it overrides, an Interface's default it has,
        // and for each implementation of an Interface, its member and the one implementing it.
        std::map<symbol_id_t, std::vector<symbol_id_t>> ties;
        const auto tie = [&](const symbol_id_t a, const symbol_id_t b) {
            ties[a].push_back(b);
            ties[b].push_back(a);
        };
        for (std::size_t i = 0; i < analysis.symbols.size(); ++i) {
            const symbol_t& symbol = analysis.symbols[i];
            if (symbol.overrides) {
                tie(static_cast<symbol_id_t>(i), *symbol.overrides);
            }

            if (symbol.default_method) {
                tie(static_cast<symbol_id_t>(i), *symbol.default_method);
            }
        }

        for (const conformance_t& conformance : analysis.conformances) {
            const std::optional<symbol_id_t> interface = query.type_symbol(conformance.interface);
            if (!interface) {
                continue;
            }

            const std::vector<symbol_id_t>& members = analysis.symbol(*interface).members;
            for (std::size_t i = 0; i < members.size() && i < conformance.members.size(); ++i) {
                tie(members[i], conformance.members[i]);
            }
        }

        std::vector<symbol_id_t> work = query.symbols_of(key);
        std::set<symbol_id_t> found(work.begin(), work.end());
        while (!work.empty()) {
            const symbol_id_t at = work.back();
            work.pop_back();
            for (const symbol_id_t next : ties[at]) {
                if (found.insert(next).second) {
                    work.push_back(next);
                }
            }
        }

        for (const symbol_id_t id : found) {
            const symbol_key_t each = query.key(id);
            if (!std::ranges::contains(out.keys, each) && !each.file.empty()) {
                out.keys.push_back(each);
                if (std::optional<std::filesystem::path> path = query.declared_in(id)) {
                    out.files.insert(std::move(*path));
                }
            }
        }
    }

    return out;
}

std::optional<std::string>
rename_conflict(const std::span<const std::shared_ptr<const compilation_t>> compilations,
                const std::span<const symbol_key_t> keys, const std::string& name) {
    for (const std::shared_ptr<const compilation_t>& compilation : compilations) {
        const analysis_t& analysis = compilation->analysis;
        const query_t query(*compilation, compilation->file);
        std::set<symbol_id_t> symbols;
        for (const symbol_key_t& key : keys) {
            const std::vector<symbol_id_t> found = query.symbols_of(key);
            symbols.insert(found.begin(), found.end());
        }

        // Where it's said: another declaration's place, as "shapes.fb:3".
        const auto place = [&](const span_t span) {
            const source_file_t& file = compilation->sources.get(span.file);
            return std::format("{}:{}", file.path(),
                               file.location_of(span.begin, column_unit_t::UTF8).line + 1);
        };
        for (const symbol_id_t id : symbols) {
            const symbol_t& symbol = analysis.symbol(id);
            // A member's name is its type's, the Classes' it extends too.
            if (symbol.owner != type_id_t::INVALID) {
                std::set<type_id_t> owners;
                type_id_t type = symbol.owner;
                while (type != type_id_t::INVALID && owners.insert(type).second) {
                    type = analysis.types.get(type).base;
                }

                for (const symbol_t& other : analysis.symbols) {
                    if (other.name == name && owners.contains(other.owner) && !other.span.empty()) {
                        return std::format("{} already has a member named '{}', at {}",
                                           query.type_name(symbol.owner), name, place(other.span));
                    }
                }

                continue;
            }

            // A variable's name is taken by another in its function, or its file's top-level code,
            // declared before it or after.
            const auto variable = [](const symbol_kind_t kind) {
                return kind == symbol_kind_t::LOCAL || kind == symbol_kind_t::CONSTANT ||
                       kind == symbol_kind_t::PARAMETER || kind == symbol_kind_t::LOOP_VARIABLE;
            };
            if (variable(symbol.kind) && !symbol.span.empty()) {
                span_t scope{ .file = symbol.span.file, .begin = 0, .end = UINT32_MAX };
                for (std::size_t i = 0; i < compilation->ast.stmt_count(); ++i) {
                    const stmt_t& statement = compilation->ast.get(static_cast<stmt_id_t>(i));
                    if (std::holds_alternative<function_stmt_t>(statement.node) &&
                        statement.span.file == symbol.span.file &&
                        statement.span.begin <= symbol.span.begin &&
                        symbol.span.end <= statement.span.end &&
                        statement.span.length() < scope.length()) {
                        scope = statement.span;
                    }
                }

                for (const symbol_t& other : analysis.symbols) {
                    if (other.name == name && variable(other.kind) &&
                        other.span.file == scope.file && scope.begin <= other.span.begin &&
                        other.span.end <= scope.end) {
                        return std::format("'{}' is declared already where '{}' is, at {}", name,
                                           symbol.name, place(other.span));
                    }
                }
            }

            // Anything else is a name that code sees where it's declared and used.
            std::vector<span_t> places{ symbol.span };
            for (const symbol_use_t& use : analysis.uses) {
                if (const std::optional<named_t> named = query.name_of(use);
                    named && named->symbol == id) {
                    places.push_back(named->span);
                }
            }

            for (const span_t span : places) {
                if (span.empty()) {
                    continue;
                }

                const query_t there(*compilation, span.file);
                if (visible_names(there, span.begin).contains(name)) {
                    return std::format("'{}' is a name already where '{}' is, at {}", name,
                                       symbol.name, place(span));
                }
            }
        }
    }

    return std::nullopt;
}

json_t rename_edit(const json_t& references, const std::string& name) {
    json_t changes = json_t::object();
    for (const json_t& location : references) {
        const std::string uri = location["uri"].get<std::string>();
        if (!changes.contains(uri)) {
            changes[uri] = json_t::array();
        }

        changes[uri].push_back({ { "range", location["range"] }, { "newText", name } });
    }

    return { { "changes", std::move(changes) } };
}

bool is_identifier(const std::string& name) {
    if (name.empty() || keyword_kind(name) || (name[0] >= '0' && name[0] <= '9')) {
        return false;
    }

    // ASCII letters, digits, and '_', as docs/spec/01-lexical.md has them.
    return std::ranges::all_of(name, [](const char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_';
    });
}

json_t document_symbols(const source_file_t& file, const positions_t& positions) {
    json_t out = json_t::array();
    for (const outline_t& symbol : outline(file)) {
        out.push_back(document_symbol(symbol, file, positions));
    }

    return out;
}

json_t workspace_symbols(const source_file_t& file, const std::string& uri,
                         const std::string& query, const positions_t& positions) {
    json_t out = json_t::array();
    collect_symbols(outline(file), {}, file, uri, query, positions, out);
    return out;
}

json_t signature_help(const query_t& query, const std::uint32_t offset,
                      const positions_t& positions) {
    // The innermost call whose parentheses `offset` is in.
    const ast_t& ast = query.ast();
    const source_file_t& source = query.source();
    std::optional<expr_id_t> best;
    for (std::size_t i = 0; i < ast.expr_count(); ++i) {
        const expr_t& expr = ast.get(static_cast<expr_id_t>(i));
        const auto* call = std::get_if<call_expr_t>(&expr.node);
        if (call == nullptr || expr.span.file != query.file()) {
            continue;
        }

        const span_t callee = ast.get(call->callee).span;
        // A call whose ')' is missing ends where its last argument does.
        const bool closed = source.slice(expr.span).ends_with(')');
        if (offset <= callee.end || offset < expr.span.begin ||
            (closed ? offset >= expr.span.end : offset > expr.span.end + 1)) {
            continue;
        }

        if (!best || expr.span.length() < ast.get(*best).span.length()) {
            best = static_cast<expr_id_t>(i);
        }
    }

    if (!best) {
        return nullptr;
    }

    const auto& call = std::get<call_expr_t>(ast.get(*best).node);
    const analysis_t& analysis = query.analysis();
    std::optional<symbol_id_t> resolved = analysis.expr(*best).symbol;
    if (!resolved) {
        resolved = analysis.expr(call.callee).symbol;
    }

    // Which argument `offset` is in: the commas before it, outside brackets, as the lexer finds
    // them, so a string's or a character's aren't counted.
    const std::uint32_t open = ast.get(call.callee).span.end;
    std::uint32_t active = 0;
    int depth = 0;
    diagnostic_bag_t ignored;
    const std::vector<std::string_view> defined = defined_symbols(build_mode_t::DEBUG);
    for (const token_t& token : lex(source, ignored, defined)) {
        if (token.span.begin <= open || token.span.begin >= offset) {
            continue;
        }

        switch (token.kind) {
        case token_kind_t::LEFT_PAREN:
        case token_kind_t::LEFT_BRACKET:
        case token_kind_t::LEFT_BRACE:
            ++depth;
            break;
        case token_kind_t::RIGHT_PAREN:
        case token_kind_t::RIGHT_BRACKET:
        case token_kind_t::RIGHT_BRACE:
            --depth;
            break;
        case token_kind_t::COMMA:
            active += depth == 0 ? 1 : 0;
            break;
        default:
            break;
        }
    }

    const std::vector<symbol_id_t> candidates = overloads(query, call, resolved);
    if (candidates.empty()) {
        return nullptr;
    }

    // The one the call resolved to, or else the first that takes as many arguments as it has.
    std::size_t chosen = 0;
    if (const auto at = std::ranges::find(candidates, resolved.value_or(symbol_id_t{}));
        resolved && at != candidates.end()) {
        chosen = static_cast<std::size_t>(at - candidates.begin());
    } else {
        const auto fits = std::ranges::find_if(candidates, [&](const symbol_id_t id) {
            const std::vector<symbol_id_t>& parameters = analysis.symbol(id).parameters;
            return active < parameters.size() ||
                   (!parameters.empty() && analysis.symbol(parameters.back()).variadic);
        });
        chosen = fits != candidates.end() ? static_cast<std::size_t>(fits - candidates.begin()) : 0;
    }

    json_t signatures = json_t::array();
    for (const symbol_id_t function : candidates) {
        signatures.push_back(signature_information(query, function, positions));
    }

    const std::size_t count = analysis.symbol(candidates[chosen]).parameters.size();
    return {
        { "signatures", std::move(signatures) },
        { "activeSignature", chosen },
        {
            "activeParameter",
            count == 0 ? 0 : std::min<std::uint32_t>(active, static_cast<std::uint32_t>(count - 1)),
        },
    };
}

json_t code_actions(const query_t& query, const span_t range, const positions_t& positions) {
    json_t out = json_t::array();
    const compilation_t& compilation = query.compilation();
    for (const diagnostic_t& diagnostic : compilation.diagnostics.diagnostics()) {
        if (diagnostic.span.file != query.file() || diagnostic.fixes.empty() ||
            diagnostic.span.begin > range.end || range.begin > diagnostic.span.end) {
            continue;
        }

        json_t changes = json_t::object();
        bool whole = true;
        for (const fix_t& fix : diagnostic.fixes) {
            const std::filesystem::path path = compilation.disk_path(fix.span.file);
            if (path.empty()) {
                whole = false;
                break;
            }

            const std::string uri = uri_of(path);
            if (!changes.contains(uri)) {
                changes[uri] = json_t::array();
            }

            changes[uri].push_back({
                { "range", positions.range(compilation.sources.get(fix.span.file), fix.span) },
                { "newText", fix.replacement },
            });
        }

        if (!whole) {
            continue;
        }

        out.push_back({
            { "title", "Fix: " + diagnostic.message },
            { "kind", "quickfix" },
            {
                "diagnostics",
                json_t::array({ diagnostic_json(compilation, diagnostic, positions) }),
            },
            { "isPreferred", true },
            { "edit", { { "changes", std::move(changes) } } },
        });
    }

    return out;
}

json_t formatting(const source_file_t& file, const std::uint32_t width,
                  const positions_t& positions) {
    const std::expected<std::string, std::vector<diagnostic_t>> formatted =
        format_source(file, width);
    if (!formatted || *formatted == file.text()) {
        return json_t::array();
    }

    const span_t whole{
        .file = file.id(),
        .begin = 0,
        .end = static_cast<std::uint32_t>(file.text().size()),
    };
    return json_t::array(
        { { { "range", positions.range(file, whole) }, { "newText", *formatted } } });
}

} // namespace fb::lsp
