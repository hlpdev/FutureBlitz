#include "fb/interface.hpp"

#include "fb/build_config.hpp"
#include "fb/lexer.hpp"
#include "fb/token.hpp"
#include "fb/utf8.hpp"

#include <algorithm>
#include <array>
#include <blake3.h>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace fb {
namespace {

/// What an interface file's first line starts with, and each module's part.
constexpr std::string_view header_start = "// fbi ";
constexpr std::string_view binary_start = "// fblib ";
constexpr std::string_view bundled_start = "// bundled";
constexpr std::string_view module_start = "// module ";
constexpr std::string_view uses_start = "// uses ";
/// First in a module's part, when it has any: the names of its top-level Package declarations,
/// which the interface file leaves out, so a message to the library's tests can say one is.
constexpr std::string_view package_start = "// package ";
/// Before each declaration, and a module's doc comment: the line of the library's source that the
/// lines after it start at, the doc comment's first, which fb doc names.
constexpr std::string_view line_start = "// line ";

/// One of the library's modules as the interface file writes it: its file's tokens, lexed for
/// the build's mode, and the hidden Imports its printed types need, by module. Making its map
/// can allocate, which clang-tidy reports as an exception escaping.
struct module_text_t { // NOLINT(bugprone-exception-escape)
    const module_t* module = nullptr;
    const source_file_t* file = nullptr;
    std::vector<token_t> tokens;
    /// Its doc comments, which the interface file keeps, for fb doc and the LSP.
    std::vector<span_t> docs;
    std::map<module_id_t, std::string> qualifiers;
};

/// A declaration the interface file can hold: a top-level one, or a member of a type, an
/// Extend, or an Extern block.
struct candidate_t {
    stmt_id_t id{};
    /// Its module, by index among the library's.
    std::size_t module = 0;
    /// For a member, the candidate it's in.
    std::optional<std::size_t> owner;
    std::string_view name;
    /// Public, a Protected member, or an Extend, which other packages see.
    bool shown = false;
    /// A member of a type or an Extend, which the interface file keeps with it, Private ones
    /// too, so programs check against them as against the source: a Private New stays one, an
    /// Interface's Methods stay implemented, and a Method can't take a Private one's name.
    bool always = false;
    bool included = false;
};

/// A Char or a String's text as a literal writes it: printable ASCII as it is, its quote and '\\'
/// after a '\\', a String's braces doubled, as interpolation needs, and the rest as \u{...}.
std::string escaped(const std::u32string_view text, const char32_t quote) {
    std::string out;
    for (const char32_t c : text) {
        if (quote == U'"' && (c == U'{' || c == U'}')) {
            out.append(2, static_cast<char>(c));
        } else if (c == quote || c == U'\\') {
            out += std::format("\\{}", static_cast<char>(c));
        } else if (c >= 0x20 && c < 0x7F) {
            out += static_cast<char>(c);
        } else {
            out += std::format("\\u{{{:X}}}", static_cast<std::uint32_t>(c));
        }
    }

    return out;
}

/// A String's code points. The text a Const holds is valid UTF-8.
std::u32string code_points(const std::string_view text) {
    std::u32string out;
    for (std::size_t at = 0; at < text.size();) {
        const decoded_t decoded = decode_utf8(text, at);
        out += decoded.code_point;
        at += decoded.length;
    }

    return out;
}

/// Whether a module's name is one an Import takes: identifiers between its dots.
bool dotted(const std::string_view name) {
    for (std::size_t begin = 0; begin <= name.size();) {
        const std::size_t end = std::min(name.find('.', begin), name.size());
        const std::string_view part = name.substr(begin, end - begin);
        const auto letter = [](const char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        };
        if (part.empty() || !letter(part.front()) || keyword_kind(part) ||
            !std::ranges::all_of(
                part, [&](const char c) { return letter(c) || (c >= '0' && c <= '9'); })) {
            return false;
        }

        begin = end + 1;
    }

    return true;
}

/// A float as the shortest decimal that reads back as it, with a '.', so it's a decimal literal.
std::string decimal_text(const double value) {
    std::array<char, 64> buffer{};
    const std::to_chars_result result =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    std::string text(buffer.data(), result.ptr);
    if (!text.contains('.')) {
        const std::size_t exponent = text.find('e');
        text.insert(exponent == std::string::npos ? text.size() : exponent, ".0");
    }

    return text;
}

/// A Const's value as a literal of its built-in type, or nullopt for one that isn't.
std::optional<std::string> literal_of(const const_value_t& value) {
    const type_id_t type = value.type;
    if (type == type_id_t::CHAR) {
        const auto* code = std::get_if<std::uint64_t>(&value.data);
        return code == nullptr
                   ? std::nullopt
                   : std::optional(std::format(
                         "'{}'", escaped(std::u32string(1, static_cast<char32_t>(*code)), U'\'')));
    }

    if (type < type_id_t::BOOL || type > type_id_t::STRING || type == type_id_t::CLONG ||
        type == type_id_t::CULONG || type == type_id_t::CWCHAR) {
        return std::nullopt;
    }

    if (const auto* flag = std::get_if<bool>(&value.data)) {
        return *flag ? "True" : "False";
    }

    if (const auto* number = std::get_if<std::int64_t>(&value.data)) {
        return std::to_string(*number);
    }

    if (const auto* number = std::get_if<std::uint64_t>(&value.data)) {
        return std::to_string(*number);
    }

    if (const auto* number = std::get_if<double>(&value.data)) {
        return decimal_text(*number);
    }

    if (const auto* text = std::get_if<std::string>(&value.data)) {
        return std::format("\"{}\"", escaped(code_points(*text), U'"'));
    }

    return std::nullopt;
}

/// Writes a library's interface file from its compilation.
class writer_t {
public:
    writer_t(const compilation_t& compilation, const package_info_t& package,
             const build_mode_t mode, diagnostic_bag_t& diagnostics)
        : m_diagnostics(diagnostics), m_ast(compilation.ast), m_analysis(compilation.analysis) {
        const std::string prefix = package.name + "/";
        const std::vector<std::string_view> defined = defined_symbols(mode);
        for (const module_t& module : m_ast.modules().modules()) {
            if (module.is_main || !module.path.starts_with(prefix)) {
                continue;
            }

            diagnostic_bag_t ignored;
            const source_file_t& file = compilation.sources.get(module.file);
            std::vector<span_t> docs;
            std::vector<token_t> tokens = lex(file, ignored, defined, &docs);
            m_modules.push_back({
                .module = &module,
                .file = &file,
                .tokens = std::move(tokens),
                .docs = std::move(docs),
            });
        }

        for (const stmt_id_t id : m_ast.items()) {
            const std::optional<std::size_t> module = module_index(m_ast.get(id).span.file);
            if (module) {
                m_items.emplace_back(id, *module);
                add_candidate(id, *module, std::nullopt);
            }
        }

        for (std::size_t i = 0; i < m_ast.expr_count(); ++i) {
            if (m_analysis.exprs[i].embeds) {
                m_embeds.push_back(static_cast<expr_id_t>(i));
            }

            // A name that analysis resolved refers to its symbol, at the name's place.
            const expr_t& expr = m_ast.get(static_cast<expr_id_t>(i));
            if (const std::optional<symbol_id_t> symbol = m_analysis.exprs[i].symbol) {
                if (std::holds_alternative<name_expr_t>(expr.node)) {
                    m_referred.emplace(place_of(expr.span), *symbol);
                } else if (const auto* member = std::get_if<member_expr_t>(&expr.node)) {
                    m_referred.emplace(place_of(member->member.span), *symbol);
                }
            }
        }

        // Where each symbol is declared, its name isn't a use of another.
        for (const symbol_t& symbol : m_analysis.symbols) {
            if (!symbol.name.empty() && symbol.span.end > symbol.span.begin) {
                m_declared.insert(place_of(symbol.span));
            }
        }
    }

    std::optional<std::string> run() {
        // Each declaration that's included brings in the names its kept text uses, which can
        // include more, until none does.
        for (bool changed = true; changed;) {
            changed = false;
            for (std::size_t i = 0; i < m_candidates.size(); ++i) {
                if (!m_candidates[i].included && wanted(m_candidates[i])) {
                    include(i);
                    changed = true;
                }
            }
        }

        std::string body;
        for (std::size_t module = 0; module < m_modules.size(); ++module) {
            const std::string text = package_names(module) + module_text(module);
            const auto lines = std::ranges::count(text, '\n');
            body += std::format("{}{} {}\n{}", module_start, m_modules[module].module->path, lines,
                                text);
        }

        if (m_failed) {
            return std::nullopt;
        }

        return body;
    }

private:
    /// The `// package` line of a module's top-level Package declarations' names, or nothing.
    [[nodiscard]] std::string package_names(const std::size_t module) const {
        std::set<std::string_view> names;
        for (const symbol_t& symbol : m_analysis.symbols) {
            if (symbol.declaration && symbol.owner == type_id_t::INVALID &&
                symbol.modifiers.visibility == visibility_t::PACKAGE &&
                module_index(symbol.span.file) == module &&
                (symbol.kind == symbol_kind_t::FUNCTION || symbol.kind == symbol_kind_t::GLOBAL ||
                 symbol.kind == symbol_kind_t::CONSTANT || symbol.kind == symbol_kind_t::TYPE ||
                 symbol.kind == symbol_kind_t::ALIAS)) {
                names.insert(symbol.name);
            }
        }

        if (names.empty()) {
            return {};
        }

        std::string line(package_start);
        for (const std::string_view name : names) {
            line += std::string(name) + " ";
        }

        line.back() = '\n';
        return line;
    }

    diagnostic_bag_t& m_diagnostics;
    const ast_t& m_ast;
    const analysis_t& m_analysis;
    std::vector<module_text_t> m_modules;
    /// Each module's top-level statements, in order, with their module.
    std::vector<std::pair<stmt_id_t, std::size_t>> m_items;
    std::vector<candidate_t> m_candidates;
    /// Each statement's candidate.
    std::map<stmt_id_t, std::size_t> m_by_statement;
    /// The names the included declarations' kept text uses that analysis didn't resolve, like a
    /// type's in a header, which reach any declaration of that name.
    std::set<std::string_view, std::less<>> m_names;
    /// What the kept text's resolved names refer to: their declarations, and each module's
    /// names, which reach a Function's overloads too.
    std::set<stmt_id_t> m_reached;
    std::set<std::pair<std::size_t, std::string_view>> m_reached_names;
    /// The symbol each resolved name refers to, and where each symbol's name is declared, by
    /// file and offset.
    std::map<std::pair<file_id_t, std::uint32_t>, symbol_id_t> m_referred;
    std::set<std::pair<file_id_t, std::uint32_t>> m_declared;
    /// The Embeds, EmbedTexts, and EmbedFolders of the program.
    std::vector<expr_id_t> m_embeds;
    /// The Global or Field whose type is being written.
    name_t m_typed;
    bool m_failed = false;

    [[nodiscard]] static std::pair<file_id_t, std::uint32_t> place_of(const span_t span) {
        return { span.file, span.begin };
    }

    [[nodiscard]] std::optional<std::size_t> module_index(const file_id_t file) const {
        for (std::size_t i = 0; i < m_modules.size(); ++i) {
            if (m_modules[i].module->file == file) {
                return i;
            }
        }

        return std::nullopt;
    }

    /// A declaration's modifiers, for the kinds that have them.
    [[nodiscard]] const modifiers_t* modifiers_of(const stmt_id_t id) const {
        return std::visit(
            [](const auto& node) -> const modifiers_t* {
                using node_t = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<node_t, function_stmt_t> ||
                              std::is_same_v<node_t, variable_stmt_t> ||
                              std::is_same_v<node_t, type_stmt_t> ||
                              std::is_same_v<node_t, property_stmt_t> ||
                              std::is_same_v<node_t, enum_stmt_t> ||
                              std::is_same_v<node_t, alias_stmt_t> ||
                              std::is_same_v<node_t, extern_stmt_t>) {
                    return &node.modifiers;
                } else {
                    return nullptr;
                }
            },
            m_ast.get(id).node);
    }

    /// Where a declaration starts: at its first attribute or modifier.
    [[nodiscard]] std::uint32_t start_of(const stmt_id_t id) const {
        const modifiers_t* modifiers = modifiers_of(id);
        return modifiers != nullptr && modifiers->start ? *modifiers->start
                                                        : m_ast.get(id).span.begin;
    }

    /// Whether the interface file keeps a whole declaration as it's written, members, bodies,
    /// and all: generic code, @Inline and Const code, an Interface, an Attribute, an Enum, an
    /// Alias, an Import, and what has no body.
    [[nodiscard]] bool verbatim(const stmt_id_t id) const {
        const stmt_node_t& node = m_ast.get(id).node;
        if (const auto* type = std::get_if<type_stmt_t>(&node)) {
            return !type->type_parameters.empty() || type->is_interface || type->is_attribute;
        }

        if (const auto* function = std::get_if<function_stmt_t>(&node)) {
            const modifiers_t& modifiers = function->modifiers;
            return !function->type_parameters.empty() || modifiers.is_inline ||
                   modifiers.is_const || modifiers.is_abstract || function->required ||
                   function->external;
        }

        if (const auto* property = std::get_if<property_stmt_t>(&node)) {
            return property->required || property->modifiers.is_abstract ||
                   property->modifiers.is_inline;
        }

        if (const auto* extend = std::get_if<extend_stmt_t>(&node)) {
            return generic_extend(*extend);
        }

        return std::holds_alternative<enum_stmt_t>(node) ||
               std::holds_alternative<alias_stmt_t>(node) ||
               std::holds_alternative<import_stmt_t>(node);
    }

    /// Whether an Extend is of generic code: of a generic type's instances, of T[], or with
    /// Where lines.
    [[nodiscard]] bool generic_extend(const extend_stmt_t& extend) const {
        if (!extend.constraints.empty()) {
            return true;
        }

        return std::ranges::any_of(extend.members, [&](const stmt_id_t member) {
            const std::optional<symbol_id_t> symbol = m_analysis.stmt(member).symbol;
            return symbol && has_parameter(m_analysis.symbol(*symbol).owner);
        });
    }

    /// Whether a type has a type parameter in it, or is a generic one's declaration.
    [[nodiscard]] bool has_parameter(const type_id_t id) const {
        if (id == type_id_t::INVALID || id < type_id_t::BOOL) {
            return false;
        }

        const type_t& type = m_analysis.types.get(id);
        if (type.kind == type_kind_t::PARAMETER) {
            return true;
        }

        if ((type.kind == type_kind_t::STRUCT || type.kind == type_kind_t::CLASS ||
             type.kind == type_kind_t::INTERFACE) &&
            type.generic == type_id_t::INVALID && !type.parameters.empty()) {
            return true;
        }

        return has_parameter(type.inner) ||
               std::ranges::any_of(type.arguments, [&](const auto a) { return has_parameter(a); });
    }

    void add_candidate(const stmt_id_t id, const std::size_t module,
                       const std::optional<std::size_t> owner) {
        const stmt_node_t& node = m_ast.get(id).node;
        const modifiers_t* modifiers = modifiers_of(id);
        const bool member = owner.has_value();
        const visibility_t visibility =
            modifiers != nullptr ? modifiers->visibility : visibility_t::PRIVATE;
        candidate_t candidate{
            .id = id,
            .module = module,
            .owner = owner,
            .shown = visibility == visibility_t::PUBLIC ||
                     (member && visibility == visibility_t::PROTECTED),
        };
        std::visit(
            [&](const auto& declared) {
                using node_t = std::decay_t<decltype(declared)>;
                if constexpr (std::is_same_v<node_t, function_stmt_t> ||
                              std::is_same_v<node_t, variable_stmt_t> ||
                              std::is_same_v<node_t, property_stmt_t> ||
                              std::is_same_v<node_t, type_stmt_t> ||
                              std::is_same_v<node_t, enum_stmt_t> ||
                              std::is_same_v<node_t, alias_stmt_t>) {
                    candidate.name = declared.name.text;
                }
            },
            node);
        // An Extend has no visibility of its own: its members do, and what it makes its type
        // implement is seen everywhere.
        candidate.shown = candidate.shown || std::holds_alternative<extend_stmt_t>(node);
        candidate.always = member && !is_block(m_candidates[*owner]);

        // An Import isn't a declaration: every one is kept, as other code can need its names. A
        // test is only fb test's, which runs the library's source.
        if (std::holds_alternative<import_stmt_t>(node) ||
            std::holds_alternative<error_stmt_t>(node) ||
            (modifiers != nullptr && modifiers->is_test)) {
            return;
        }

        const std::size_t index = m_candidates.size();
        m_candidates.push_back(candidate);
        m_by_statement.emplace(id, index);
        if (verbatim(id)) {
            return;
        }

        const block_t* members = nullptr;
        if (const auto* type = std::get_if<type_stmt_t>(&node)) {
            members = &type->members;
        } else if (const auto* extend = std::get_if<extend_stmt_t>(&node)) {
            members = &extend->members;
        } else if (const auto* block = std::get_if<extern_stmt_t>(&node)) {
            members = &block->functions;
        }

        if (members != nullptr) {
            for (const stmt_id_t each : *members) {
                add_candidate(each, module, index);
            }
        }
    }

    /// Whether a candidate is an Extern block, whose Functions decide whether it's kept.
    [[nodiscard]] bool is_block(const candidate_t& candidate) const {
        return std::holds_alternative<extern_stmt_t>(m_ast.get(candidate.id).node);
    }

    [[nodiscard]] bool wanted(const candidate_t& candidate) const {
        if (is_block(candidate) && !verbatim(candidate.id)) {
            // A block's kept with the members it keeps.
            return false;
        }

        const bool in_block = candidate.owner && is_block(m_candidates[*candidate.owner]);
        const bool owner_kept =
            !candidate.owner || in_block || m_candidates[*candidate.owner].included;
        if (!owner_kept) {
            return false;
        }

        return candidate.shown || candidate.always || m_reached.contains(candidate.id) ||
               (!candidate.name.empty() &&
                (m_names.contains(candidate.name) ||
                 (!candidate.owner &&
                  m_reached_names.contains({ candidate.module, candidate.name }))));
    }

    void include(const std::size_t index) {
        m_candidates[index].included = true;
        if (const std::optional<std::size_t> owner = m_candidates[index].owner;
            owner && is_block(m_candidates[*owner]) && !m_candidates[*owner].included) {
            m_candidates[*owner].included = true;
            note_names(*owner);
        }

        note_names(index);
    }

    /// The parts of a declaration's text the interface file keeps, from its file.
    [[nodiscard]] std::vector<std::pair<std::uint32_t, std::uint32_t>>
    kept_ranges(const std::size_t index) const {
        const stmt_id_t id = m_candidates[index].id;
        const stmt_t& stmt = m_ast.get(id);
        const std::uint32_t start = start_of(id);
        if (verbatim(id)) {
            return { { start, stmt.span.end } };
        }

        const stmt_node_t& node = stmt.node;
        if (const auto* function = std::get_if<function_stmt_t>(&node)) {
            return { { start, function->header_end } };
        }

        if (const auto* type = std::get_if<type_stmt_t>(&node)) {
            return { { start, type->header_end } };
        }

        if (const auto* property = std::get_if<property_stmt_t>(&node)) {
            return { { start, property->header_end } };
        }

        if (const auto* extend = std::get_if<extend_stmt_t>(&node)) {
            return { { start, extend->header_end } };
        }

        if (const auto* block = std::get_if<extern_stmt_t>(&node)) {
            return { { start, block->header_end } };
        }

        if (const auto* variable = std::get_if<variable_stmt_t>(&node)) {
            // A Field's default, which the code that makes its Struct or Class sets, is written
            // as its value when it's one, since only that's known at compile time.
            if (variable->kind == variable_kind_t::FIELD && !variable->modifiers.is_static) {
                const std::uint32_t written =
                    variable->type ? m_ast.get(*variable->type).span.end : variable->name.span.end;
                return { { start, const_literal(id) ? written : stmt.span.end } };
            }

            if (variable->kind == variable_kind_t::CONSTANT) {
                // A Const written as its value keeps only its name.
                return { { start, const_literal(id) ? variable->name.span.end : stmt.span.end } };
            }

            const std::uint32_t end =
                variable->type ? m_ast.get(*variable->type).span.end : variable->name.span.end;
            return { { start, end } };
        }

        return { { start, stmt.span.end } };
    }

    /// A Const's value, or a Field's default, as a literal, when it's known and of a built-in
    /// type.
    [[nodiscard]] std::optional<std::string> const_literal(const stmt_id_t id) const {
        const std::optional<symbol_id_t> symbol = m_analysis.stmt(id).symbol;
        if (!symbol) {
            return std::nullopt;
        }

        const std::optional<const_value_t>& value = m_analysis.symbol(*symbol).value;
        return value ? literal_of(*value) : std::nullopt;
    }

    /// The tokens of a module in [begin, end).
    [[nodiscard]] static std::span<const token_t>
    tokens_in(const module_text_t& module, const std::uint32_t begin, const std::uint32_t end) {
        const auto first = std::ranges::lower_bound(
            module.tokens, begin, {}, [](const token_t& token) { return token.span.begin; });
        auto last = first;
        while (last != module.tokens.end() && last->kind != token_kind_t::END_OF_FILE &&
               last->span.end <= end) {
            ++last;
        }

        // A declaration's span can take the line break after it, which its caller writes.
        while (last != first && std::prev(last)->kind == token_kind_t::NEWLINE) {
            --last;
        }

        return { first, last };
    }

    void note_names(const std::size_t index) {
        const module_text_t& module = m_modules[m_candidates[index].module];
        for (const auto& [begin, end] : kept_ranges(index)) {
            for (const token_t& token : tokens_in(module, begin, end)) {
                if (token.kind != token_kind_t::IDENTIFIER) {
                    continue;
                }

                // A name analysis resolved reaches what it refers to; a declaration's own, a
                // member's, a parameter's, or a Local's, reaches nothing; and the rest, by
                // spelling, anything of that name.
                const auto place = std::pair(module.module->file, token.span.begin);
                if (const auto found = m_referred.find(place); found != m_referred.end()) {
                    reach(found->second, module.file->slice(token.span));
                } else if (!m_declared.contains(place)) {
                    m_names.insert(module.file->slice(token.span));
                }
            }
        }

        // A Global's type that isn't written is printed, and names what it holds.
        const stmt_id_t id = m_candidates[index].id;
        const auto* variable = std::get_if<variable_stmt_t>(&m_ast.get(id).node);
        const std::optional<symbol_id_t> symbol = m_analysis.stmt(id).symbol;
        if (variable != nullptr && !variable->type && symbol &&
            (variable->kind == variable_kind_t::GLOBAL || variable->modifiers.is_static)) {
            note_type_names(m_analysis.symbol(*symbol).type);
        }
    }

    /// Reaches a symbol's declaration, and the other declarations of its name in its module. A
    /// member reaches its type too, and what has no declaration of its own, like a Struct's
    /// constructor, reaches by the name it's `spelled`.
    void reach(const symbol_id_t id, const std::string_view spelled) {
        const symbol_t& symbol = m_analysis.symbol(id);
        note_type_names(symbol.owner);
        if (!symbol.declaration) {
            m_names.insert(spelled);
            return;
        }

        if (const auto found = m_by_statement.find(*symbol.declaration);
            found != m_by_statement.end()) {
            m_reached.insert(*symbol.declaration);
            if (!m_candidates[found->second].owner) {
                m_reached_names.emplace(m_candidates[found->second].module, symbol.name);
            }
        }
    }

    /// The names of the Structs, Classes, Interfaces, and Enums in a type.
    void note_type_names(const type_id_t id) {
        if (id == type_id_t::INVALID || id < type_id_t::BOOL) {
            return;
        }

        const type_t& type = m_analysis.types.get(id);
        if (type.kind == type_kind_t::STRUCT || type.kind == type_kind_t::CLASS ||
            type.kind == type_kind_t::INTERFACE || type.kind == type_kind_t::ENUM) {
            const type_id_t declared = type.generic != type_id_t::INVALID ? type.generic : id;
            const std::string_view name = m_analysis.types.get(declared).name;
            m_names.insert(name.substr(0, name.find('<')));
        }

        note_type_names(type.inner);
        for (const type_id_t each : type.arguments) {
            note_type_names(each);
        }

        for (const type_id_t each : type.parameters) {
            note_type_names(each);
        }

        for (const type_id_t each : type.throws) {
            note_type_names(each);
        }

        if (type.kind == type_kind_t::TUPLE) {
            for (const field_t& field : type.fields) {
                note_type_names(field.type);
            }
        }
    }

    /// The text of [begin, end) of a module as its tokens have it: with the indentation of its
    /// first line, the spacing between its tokens, line breaks, and the doc comments on lines of
    /// their own, but without other comments or the lines an #If leaves out.
    [[nodiscard]] static std::string text_of(const module_text_t& module, const std::uint32_t begin,
                                             const std::uint32_t end) {
        const std::span<const token_t> tokens = tokens_in(module, begin, end);
        const std::string_view source = module.file->text();
        std::string out(indentation(module, begin));
        for (std::size_t i = 0; i < tokens.size(); ++i) {
            if (i > 0) {
                const std::string_view gap = source.substr(
                    tokens[i - 1].span.end, tokens[i].span.begin - tokens[i - 1].span.end);
                const std::size_t line = gap.find_last_of("\r\n");
                const std::string_view after =
                    line == std::string_view::npos ? gap : gap.substr(line + 1);
                const bool blank = after.find_first_not_of(" \t") == std::string_view::npos;
                if (tokens[i - 1].kind == token_kind_t::NEWLINE) {
                    out += docs_between(module, tokens[i - 1].span.end, tokens[i].span.begin);
                    out += blank ? after : std::string_view();
                } else if (blank && line == std::string_view::npos) {
                    out += gap;
                } else if (tokens[i].kind != token_kind_t::NEWLINE) {
                    out += ' ';
                }
            }

            out += tokens[i].kind == token_kind_t::NEWLINE ? std::string_view("\n")
                                                           : module.file->slice(tokens[i].span);
        }

        return out;
    }

    /// The doc comments in [begin, end) of a module that are first on their lines, each a line
    /// with its indentation.
    [[nodiscard]] static std::string
    docs_between(const module_text_t& module, const std::uint32_t begin, const std::uint32_t end) {
        std::string out;
        const auto first = std::ranges::lower_bound(module.docs, begin, {}, &span_t::begin);
        for (auto at = first; at != module.docs.end() && at->end <= end; ++at) {
            const std::string_view source = module.file->text().substr(0, at->begin);
            const std::size_t line = source.find_last_of("\r\n");
            const std::string_view before =
                line == std::string_view::npos ? source : source.substr(line + 1);
            if (before.find_first_not_of(" \t") == std::string_view::npos) {
                out += std::format("{}{}\n", before, module.file->slice(*at));
            }
        }

        return out;
    }

    /// A declaration's own doc comment, or with no `id`, its module's, as "///" or "//!" lines
    /// after `indent`.
    [[nodiscard]] std::string doc_lines(const module_text_t& module,
                                        const std::optional<stmt_id_t> id,
                                        const std::string_view indent) const {
        const doc_comment_t* doc = id ? m_ast.doc(*id) : m_ast.module_doc(module.module->file);
        if (doc == nullptr) {
            return {};
        }

        const std::string_view marker = id ? "///" : "//!";
        std::string out;
        for (const auto part : std::views::split(std::string_view(doc->text), '\n')) {
            const std::string_view line(part.begin(), part.end());
            out += std::format("{}{}{}{}\n", indent, marker, line.empty() ? "" : " ", line);
        }

        return out;
    }

    /// The "// line" comment before a declaration, or with no `id`, before its module's doc
    /// comment, which has none without one.
    [[nodiscard]] std::string line_comment(const module_text_t& module,
                                           const std::optional<stmt_id_t> id,
                                           const std::string_view indent) const {
        const doc_comment_t* doc = id ? m_ast.doc(*id) : m_ast.module_doc(module.module->file);
        std::optional<std::uint32_t> offset;
        if (doc != nullptr && !doc->lines.empty()) {
            offset = doc->lines.front().begin;
        } else if (id) {
            offset = start_of(*id);
        }

        if (!offset) {
            return {};
        }

        return std::format("{}{}{}\n", indent, line_start, module.file->line_of(*offset) + 1);
    }

    /// The spaces and tabs before `offset` on its line.
    [[nodiscard]] static std::string_view indentation(const module_text_t& module,
                                                      const std::uint32_t offset) {
        const std::string_view source = module.file->text().substr(0, offset);
        const std::size_t line = source.find_last_of("\r\n");
        const std::string_view before =
            line == std::string_view::npos ? source : source.substr(line + 1);
        return before.find_first_not_of(" \t") == std::string_view::npos ? before
                                                                         : std::string_view();
    }

    /// A module's part of the interface file.
    std::string module_text(const std::size_t index) {
        const module_text_t& module = m_modules[index];
        std::string imports;
        std::string declarations;
        for (const auto& [id, at] : m_items) {
            if (at != index) {
                continue;
            }

            if (std::holds_alternative<import_stmt_t>(m_ast.get(id).node)) {
                imports += text_of(module, start_of(id), m_ast.get(id).span.end) + "\n";
            } else if (const auto found = m_by_statement.find(id);
                       found != m_by_statement.end() && m_candidates[found->second].included) {
                declarations += declaration_text(found->second);
            }
        }

        // The Imports the printed types need come after the module's own.
        for (const auto& [target, qualifier] : module.qualifiers) {
            imports += std::format("Import {} As {}\n", import_name(module, target), qualifier);
        }

        return line_comment(module, std::nullopt, "") + doc_lines(module, std::nullopt, "") +
               imports + declarations;
    }

    /// What an Import in `module` writes for `target`: a module of another package by its
    /// dotted name, and one of the library's own by its path from the module's folder, which
    /// any folder's name can be in.
    [[nodiscard]] std::string import_name(const module_text_t& module,
                                          const module_id_t target) const {
        const module_t& imported = m_ast.modules().get(target);
        if (imported.package != module.module->package) {
            return imported.name;
        }

        std::vector<std::string_view> from;
        for (std::size_t begin = 0; begin <= module.module->path.size();) {
            const std::size_t end =
                std::min(module.module->path.find('/', begin), module.module->path.size());
            from.emplace_back(std::string_view(module.module->path).substr(begin, end - begin));
            begin = end + 1;
        }

        // From the module's folder up to its package's, the first part of their paths, then
        // down.
        from.pop_back();
        std::string path = from.size() > 1 ? "" : "./";
        for (std::size_t i = 1; i < from.size(); ++i) {
            path += "../";
        }

        const std::string_view file =
            std::string_view(imported.path).substr(imported.path.find('/') + 1);
        return std::format("\"{}{}\"", path,
                           file.ends_with(".fb") ? file.substr(0, file.size() - 3) : file);
    }

    /// A declaration as the interface file writes it, with its doc comment and its line break.
    std::string declaration_text(const std::size_t index) {
        const candidate_t& candidate = m_candidates[index];
        const module_text_t& module = m_modules[candidate.module];
        const std::string_view indent = indentation(module, start_of(candidate.id));
        return line_comment(module, candidate.id, indent) +
               doc_lines(module, candidate.id, indent) + undocumented_text(index);
    }

    /// declaration_text() without its doc comment.
    std::string undocumented_text(const std::size_t index) {
        const candidate_t& candidate = m_candidates[index];
        module_text_t& module = m_modules[candidate.module];
        const stmt_id_t id = candidate.id;
        const stmt_t& stmt = m_ast.get(id);
        const std::uint32_t start = start_of(id);
        check_embeds(candidate);
        if (verbatim(id)) {
            return text_of(module, start, stmt.span.end) + "\n";
        }

        const std::string indent(indentation(module, start));
        const std::string first = text_of(module, start, kept_ranges(index).front().second);
        const auto members = [&](const block_t& block) {
            std::string text;
            for (const stmt_id_t member : block) {
                if (const auto found = m_by_statement.find(member);
                    found != m_by_statement.end() && m_candidates[found->second].included) {
                    text += declaration_text(found->second);
                }
            }

            return text;
        };

        return std::visit(
            [&](const auto& node) -> std::string {
                using node_t = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<node_t, function_stmt_t>) {
                    return main_thread_only(indent, { id }) + first.substr(indent.size()) +
                           " => ...\n";
                } else if constexpr (std::is_same_v<node_t, property_stmt_t>) {
                    std::string text =
                        std::format("{}{}\n{}    Get => ...\n",
                                    main_thread_only(indent, { node.getter, node.setter }),
                                    first.substr(indent.size()), indent);
                    if (node.setter) {
                        const auto& setter =
                            std::get<function_stmt_t>(m_ast.get(*node.setter).node);
                        text += std::format("{}    Set({}) => ...\n", indent,
                                            setter.parameters.front().name.text);
                    }

                    return text + std::format("{}End Property\n", indent);
                } else if constexpr (std::is_same_v<node_t, type_stmt_t>) {
                    return std::format("{}\n{}{}End {}\n", first, members(node.members), indent,
                                       node.is_class ? "Class" : "Struct");
                } else if constexpr (std::is_same_v<node_t, extend_stmt_t>) {
                    return std::format("{}\n{}{}End Extend\n", first, members(node.members),
                                       indent);
                } else if constexpr (std::is_same_v<node_t, extern_stmt_t>) {
                    return std::format("{}\n{}{}End Extern\n", first, members(node.functions),
                                       indent);
                } else if constexpr (std::is_same_v<node_t, variable_stmt_t>) {
                    return variable_text(index, node, first, module);
                } else {
                    return first + "\n";
                }
            },
            stmt.node);
    }

    /// "@MainThreadOnly("why") " after `indent`, for what of the code a declaration keeps in the
    /// binary only the main thread can run, as analysis found it, or `indent` alone: one reason
    /// for a function, or a Property's Get and Set, its Set's after "Set:".
    [[nodiscard]] std::string
    main_thread_only(const std::string& indent,
                     const std::vector<std::optional<stmt_id_t>>& parts) const {
        std::vector<std::string> reasons;
        for (std::size_t i = 0; i < parts.size(); ++i) {
            const std::optional<stmt_id_t>& part = parts[i];
            const std::optional<symbol_id_t> symbol =
                part ? m_analysis.stmt(*part).symbol : std::nullopt;
            const auto found = symbol ? m_analysis.main_thread_only.find(*symbol)
                                      : m_analysis.main_thread_only.end();
            if (found == m_analysis.main_thread_only.end()) {
                continue;
            }

            std::string_view label;
            if (parts.size() > 1) {
                label = i == 0 ? "Get: " : "Set: ";
            }

            reasons.push_back(
                std::format("{}\"{}\"", label, escaped(code_points(found->second), U'"')));
        }

        if (reasons.empty()) {
            return indent;
        }

        std::string text = indent + "@MainThreadOnly(";
        for (std::size_t i = 0; i < reasons.size(); ++i) {
            text += (i == 0 ? "" : ", ") + reasons[i];
        }

        return text + ") ";
    }

    /// A Global, Static Field, Const, or Field, whose default, which a program sets as it makes
    /// the Field's Struct or Class, is its value.
    std::string variable_text(const std::size_t index, const variable_stmt_t& node,
                              const std::string& first, module_text_t& module) {
        const stmt_id_t id = m_candidates[index].id;
        const std::optional<symbol_id_t> symbol = m_analysis.stmt(id).symbol;
        const type_id_t type = symbol ? m_analysis.symbol(*symbol).type : type_id_t::INVALID;
        if (node.kind == variable_kind_t::FIELD && !node.modifiers.is_static) {
            const std::optional<std::string> literal = const_literal(id);
            if (!literal) {
                return first + "\n";
            }

            return node.type
                       ? std::format("{} = {}\n", first, *literal)
                       : std::format("{}: {} = {}\n", first, m_analysis.types.name(type), *literal);
        }

        if (node.kind == variable_kind_t::CONSTANT) {
            const std::optional<std::string> literal = const_literal(id);
            return literal
                       ? std::format("{}: {} = {}\n", first, m_analysis.types.name(type), *literal)
                       : first + "\n";
        }

        if (node.type) {
            return first + " = ...\n";
        }

        m_typed = node.name;
        return std::format("{}: {} = ...\n", first, type_text(module, type));
    }

    /// A type as source in `module` writes it, each Struct, Class, Interface, and Enum of
    /// another module after the qualifier of a hidden Import of that module.
    std::string type_text(module_text_t& module, const type_id_t id) {
        const type_t& type = m_analysis.types.get(id);
        const auto list = [&](const std::vector<type_id_t>& types) {
            std::string text;
            for (std::size_t i = 0; i < types.size(); ++i) {
                text += std::format("{}{}", i == 0 ? "" : ", ", listed(module, types[i]));
            }

            return text;
        };

        switch (type.kind) {
        case type_kind_t::STRUCT:
        case type_kind_t::CLASS:
        case type_kind_t::INTERFACE:
        case type_kind_t::ENUM: {
            // The built-in Object is in no module, as Scheduler isn't.
            if (id == m_analysis.interfaces.object) {
                return builtin_name(module, "Object");
            }

            const type_id_t declared = type.generic != type_id_t::INVALID ? type.generic : id;
            const std::string_view name = m_analysis.types.get(declared).name;
            std::string text =
                qualifier(module, declared) + std::string(name.substr(0, name.find('<')));
            return type.generic == type_id_t::INVALID
                       ? text
                       : std::format("{}<{}>", text, list(type.arguments));
        }
        case type_kind_t::NULLABLE:
            return std::format("{}?", suffixed(module, type.inner));
        case type_kind_t::ARRAY:
            return std::format("{}[]", suffixed(module, type.inner));
        case type_kind_t::FIXED_ARRAY: {
            std::string text = std::format("{}[", suffixed(module, type.inner));
            for (std::size_t i = 0; i < type.sizes.size(); ++i) {
                text += std::format("{}{}", i == 0 ? "" : ", ", type.sizes[i]);
            }

            return text + "]";
        }
        case type_kind_t::WEAK:
            return std::format("{}<{}>", builtin_name(module, "Weak"),
                               type_text(module, type.inner));
        case type_kind_t::PTR:
            return std::format("{}<{}>", builtin_name(module, "Ptr"),
                               type.inner == type_id_t::NONE ? std::string("Void")
                                                             : type_text(module, type.inner));
        case type_kind_t::ATOMIC:
            return std::format("{}<{}>", builtin_name(module, "Atomic"),
                               type_text(module, type.inner));
        case type_kind_t::MUTEX:
            return std::format("{}<{}>", builtin_name(module, "Mutex"),
                               type_text(module, type.inner));
        case type_kind_t::CHANNEL:
            return std::format("{}<{}>", builtin_name(module, "Channel"),
                               type_text(module, type.inner));
        case type_kind_t::TASK: {
            const std::string task = builtin_name(module, "Task");
            std::string text = type.inner == type_id_t::NONE
                                   ? task
                                   : std::format("{}<{}>", task, type_text(module, type.inner));
            return type.throws.empty() ? text
                                       : std::format("{} Throws {}", text, list(type.throws));
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
        case type_kind_t::CALLBACK: {
            std::string_view word = type.sendable ? "Sendable Func" : "Func";
            if (type.kind == type_kind_t::CALLBACK) {
                word = "Callback";
            }

            // A result that ends in a Task with a Throws is in parentheses, or the Throws would be
            // the Func's.
            const std::string throws =
                type.throws.empty() ? std::string() : " Throws " + list(type.throws);
            return type.inner == type_id_t::NONE
                       ? std::format("{}({}){}", word, list(type.parameters), throws)
                       : std::format("{}({}): {}{}", word, list(type.parameters),
                                     listed(module, type.inner), throws);
        }
        case type_kind_t::SCHEDULER:
            return builtin_name(module, "Scheduler");
        case type_kind_t::THREAD:
            return builtin_name(module, "Thread");
        default:
            return type.name;
        }
    }

    /// A type before a "?" or "[]", in parentheses when it's a Func that gives something or a
    /// Task with a Throws, as type_table_t::suffixed() has it.
    std::string suffixed(module_text_t& module, const type_id_t id) {
        const type_t& type = m_analysis.types.get(id);
        const bool wrapped =
            ((type.kind == type_kind_t::FUNCTION || type.kind == type_kind_t::CALLBACK) &&
             type.inner != type_id_t::NONE) ||
            (type.kind == type_kind_t::TASK && !type.throws.empty());
        return wrapped ? std::format("({})", type_text(module, id)) : type_text(module, id);
    }

    /// A type in a list, in parentheses when it ends in a Task with a Throws, as
    /// type_table_t::listed() has it.
    std::string listed(module_text_t& module, const type_id_t id) {
        type_id_t last = id;
        while (last != type_id_t::INVALID &&
               (m_analysis.types.get(last).kind == type_kind_t::FUNCTION ||
                m_analysis.types.get(last).kind == type_kind_t::CALLBACK)) {
            last = m_analysis.types.get(last).inner;
        }

        const bool wrapped = last != type_id_t::INVALID &&
                             m_analysis.types.get(last).kind == type_kind_t::TASK &&
                             !m_analysis.types.get(last).throws.empty();
        return wrapped ? std::format("({})", type_text(module, id)) : type_text(module, id);
    }

    /// A built-in type's name, `name`, which the interface file can write only where the
    /// module's code sees no type of that name that would take its place, its own or one an
    /// Import brings in, as analysis found them.
    std::string builtin_name(module_text_t& module, const std::string_view name) {
        const module_table_t& modules = m_ast.modules();
        const std::optional<module_id_t> here = modules.of(module.module->file);
        if (!here || std::to_underlying(*here) >= m_analysis.builtin_hiders.size()) {
            return std::string(name);
        }

        const auto& hiders = m_analysis.builtin_hiders[std::to_underlying(*here)];
        if (const auto hider = hiders.find(name); hider != hiders.end()) {
            const symbol_t& hiding = m_analysis.symbols[std::to_underlying(hider->second)];
            const std::optional<module_id_t> from = modules.of(hiding.span.file);
            std::string whose = "an imported";
            if (from == here) {
                whose = "the module's own";
            } else if (from) {
                whose = std::format("{}'s", modules.get(*from).name);
            }

            m_diagnostics.error(m_typed.span,
                                "this library's interface file names the built-in {} for '{}', "
                                "which {} {} hides; name that type another way",
                                name, m_typed.text, whose, name);
            m_failed = true;
        }

        return std::string(name);
    }

    /// The qualifier a declared type takes in `module`: none in its own module, or else a
    /// hidden Import's, "__fbi1." and so on, of the module that declares it.
    std::string qualifier(module_text_t& module, const type_id_t declared) {
        const std::string_view name = m_analysis.types.get(declared).module;
        const auto& modules = m_ast.modules().modules();
        const auto found = std::ranges::find(modules, name, &module_t::name);
        if (found == modules.end() || &*found == module.module) {
            return {};
        }

        // Another package's module is imported by its dotted name, which has to be one.
        if (found->package != module.module->package && !dotted(found->name)) {
            m_diagnostics.error(m_typed.span,
                                "the interface file can't write the type of '{}', since its "
                                "module, {}, has no name an Import takes; write the type",
                                m_typed.text, found->name);
            m_failed = true;
        }

        const auto target = static_cast<module_id_t>(found - modules.begin());
        if (const auto known = module.qualifiers.find(target); known != module.qualifiers.end()) {
            return known->second + ".";
        }

        // A name the module's own text doesn't use, which its code could have.
        std::string chosen;
        for (std::size_t n = module.qualifiers.size() + 1; chosen.empty(); ++n) {
            chosen = std::format("__fbi{}", n);
            const auto used = [&](const token_t& token) {
                return token.kind == token_kind_t::IDENTIFIER &&
                       module.file->slice(token.span) == chosen;
            };
            if (std::ranges::any_of(module.tokens, used) ||
                std::ranges::any_of(module.qualifiers,
                                    [&](const auto& entry) { return entry.second == chosen; })) {
                chosen.clear();
            }
        }

        module.qualifiers.emplace(target, chosen);
        return chosen + ".";
    }

    /// Reports each Embed in the code a declaration keeps, which a program compiling it
    /// couldn't read.
    void check_embeds(const candidate_t& candidate) {
        const stmt_t& stmt = m_ast.get(candidate.id);
        const auto index = static_cast<std::size_t>(&candidate - m_candidates.data());
        for (const auto& [begin, end] : kept_ranges(index)) {
            for (const expr_id_t expr : m_embeds) {
                const span_t span = m_ast.get(expr).span;
                if (span.file == stmt.span.file && span.begin >= begin && span.end <= end) {
                    m_diagnostics.error(
                        span, "the library's interface file keeps this code, which programs "
                              "compile themselves, so it can't embed the library's files; embed "
                              "them in code that stays in the library");
                    m_failed = true;
                }
            }
        }
    }
};

/// Splits text into its lines, without their line breaks, "\n" or "\r\n", as a copy that was
/// given Windows' line breaks has them.
std::vector<std::string_view> lines_of(const std::string_view text) {
    std::vector<std::string_view> lines;
    std::size_t begin = 0;
    while (begin < text.size()) {
        const std::size_t end = std::min(text.find('\n', begin), text.size());
        std::string_view line = text.substr(begin, end - begin);
        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }

        lines.push_back(line);
        begin = end + 1;
    }

    return lines;
}

/// A digest's hex digits.
std::string hex_of(const std::array<std::uint8_t, 16>& digest) {
    std::string hex;
    for (const std::uint8_t byte : digest) {
        hex += std::format("{:02x}", byte);
    }

    return hex;
}

/// An interface file's body as its ABI hash covers it: without its doc comments' lines, and its
/// modules' lines without how many lines each has, which the doc comments count in, so a change
/// to a doc alone is the same build.
std::string undocumented(const std::string_view body) {
    const source_file_t file(file_id_t{}, "", std::string(body));
    diagnostic_bag_t ignored;
    trivia_t trivia;
    static_cast<void>(lex_layout(file, ignored, trivia));
    const std::string_view text = file.text();
    std::string out;
    std::size_t kept = 0;
    for (const span_t comment : trivia.comments) {
        const std::size_t line =
            text.find_last_of("\r\n", comment.begin == 0 ? 0 : comment.begin - 1);
        const std::size_t start =
            comment.begin == 0 || line == std::string_view::npos ? 0 : line + 1;
        if (start < kept || text.substr(start, comment.begin - start).find_first_not_of(" \t") !=
                                std::string_view::npos) {
            continue;
        }

        const std::string_view written = file.slice(comment);
        const bool doc = (written.starts_with("///") && !written.starts_with("////")) ||
                         written.starts_with("//!");
        if (doc) {
            // The whole line, its line break too.
            std::size_t end = comment.end;
            if (end < text.size() && text[end] == '\r') {
                ++end;
            }

            if (end < text.size() && text[end] == '\n') {
                ++end;
            }
            out += text.substr(kept, start - kept);
            kept = end;
        } else if (written.starts_with(module_start)) {
            const std::size_t space = written.rfind(' ');
            out += text.substr(kept, comment.begin + space - kept);
            kept = comment.end;
        }
    }

    out += text.substr(kept);
    return out;
}

} // namespace

std::string abi_hash(const std::string_view body, const std::string_view code,
                     const build_mode_t mode) {
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    const std::string build =
        std::format("{}\n{}\n{}\n", build_config::version, build_target(), mode_folder(mode));
    blake3_hasher_update(&hasher, build.data(), build.size());
    const std::string hashed = undocumented(body);
    blake3_hasher_update(&hasher, hashed.data(), hashed.size());
    const std::string size = std::format("\n{}\n", code.size());
    blake3_hasher_update(&hasher, size.data(), size.size());
    blake3_hasher_update(&hasher, code.data(), code.size());
    std::array<std::uint8_t, 16> digest{};
    blake3_hasher_finalize(&hasher, digest.data(), digest.size());
    return hex_of(digest);
}

std::string source_build(std::vector<library_file_t> files) {
    std::ranges::sort(files, {}, &library_file_t::path);
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    for (const library_file_t& file : files) {
        const std::string sizes = std::format("{} {}\n", file.path.size(), file.text.size());
        blake3_hasher_update(&hasher, sizes.data(), sizes.size());
        blake3_hasher_update(&hasher, file.path.data(), file.path.size());
        blake3_hasher_update(&hasher, file.text.data(), file.text.size());
    }

    std::array<std::uint8_t, 16> digest{};
    blake3_hasher_finalize(&hasher, digest.data(), digest.size());
    return "source:" + hex_of(digest);
}

std::string_view build_target() {
#ifdef _WIN32
    return "windows-x64";
#elif defined(__APPLE__) && defined(__aarch64__)
    return "macos-arm64";
#elif defined(__APPLE__)
    return "macos-x64";
#elif defined(__aarch64__)
    return "linux-arm64";
#else
    return "linux-x64";
#endif
}

std::string_view mode_folder(const build_mode_t mode) {
    return mode == build_mode_t::RELEASE ? "release" : "debug";
}

std::optional<std::string> write_interface(const compilation_t& compilation,
                                           const build_mode_t mode, diagnostic_bag_t& diagnostics,
                                           const std::string_view code) {
    if (!compilation.package) {
        return std::nullopt;
    }

    const package_info_t& package = *compilation.package;
    const std::optional<std::string> modules =
        writer_t(compilation, package, mode, diagnostics).run();
    if (!modules) {
        return std::nullopt;
    }

    // The builds it's built with, which a program that links it has to have, are its ABI's too.
    std::vector<package_build_t> builds = compilation.builds;
    std::ranges::sort(builds, {}, &package_build_t::name);
    std::string body;
    for (const package_build_t& build : builds) {
        if (build.name != package.name) {
            body += std::format("{}{} {} {}\n", uses_start, build.name, build.version, build.build);
        }
    }

    body += *modules;
    return std::format("{}{} {} {}\n{}", header_start, package.name, package.version,
                       abi_hash(body, code, mode), body);
}

std::string write_binary(const library_binary_t& binary) {
    std::string bundled(bundled_start);
    for (const std::string& name : binary.bundled) {
        bundled += " " + name;
    }

    return std::format("{}{} {} {}\n{}\n{}", binary_start, binary.header.name,
                       binary.header.version, binary.header.hash, bundled, binary.code);
}

std::optional<library_binary_t> read_binary(const std::string_view bytes) {
    const std::size_t first = bytes.find('\n');
    const std::size_t second =
        first == std::string_view::npos ? first : bytes.find('\n', first + 1);
    if (second == std::string_view::npos || !bytes.starts_with(binary_start)) {
        return std::nullopt;
    }

    // Its first line is an interface file's but for its word.
    const std::optional<interface_header_t> header = read_interface_header(std::format(
        "{}{}", header_start, bytes.substr(binary_start.size(), first - binary_start.size())));
    const std::string_view line = bytes.substr(first + 1, second - first - 1);
    if (!header || !line.starts_with(bundled_start)) {
        return std::nullopt;
    }

    library_binary_t binary{ .header = *header, .code = std::string(bytes.substr(second + 1)) };
    std::string_view rest = line.substr(bundled_start.size());
    while (!rest.empty()) {
        rest.remove_prefix(1);
        const std::size_t end = std::min(rest.find(' '), rest.size());
        binary.bundled.emplace_back(rest.substr(0, end));
        rest.remove_prefix(end);
    }

    return binary;
}

std::optional<interface_header_t> read_interface_header(const std::string_view text) {
    std::string_view line = text.substr(0, text.find('\n'));
    if (line.ends_with('\r')) {
        line.remove_suffix(1);
    }

    if (!line.starts_with(header_start)) {
        return std::nullopt;
    }

    std::vector<std::string_view> words;
    std::string_view rest = line.substr(header_start.size());
    while (!rest.empty()) {
        const std::size_t end = std::min(rest.find(' '), rest.size());
        words.push_back(rest.substr(0, end));
        rest = rest.substr(std::min(end + 1, rest.size()));
    }

    if (words.size() != 3 || words[2].size() != 32) {
        return std::nullopt;
    }

    return interface_header_t{
        .name = std::string(words[0]),
        .version = std::string(words[1]),
        .hash = std::string(words[2]),
    };
}

std::optional<std::vector<package_build_t>> interface_uses(const std::string_view text) {
    const std::vector<std::string_view> lines = lines_of(text);
    std::vector<package_build_t> builds;
    for (std::size_t at = 1; at < lines.size() && lines[at].starts_with(uses_start); ++at) {
        std::vector<std::string_view> words;
        for (const auto word : std::views::split(lines[at].substr(uses_start.size()), ' ')) {
            words.emplace_back(word.begin(), word.end());
        }

        if (words.size() != 3) {
            return std::nullopt;
        }

        builds.push_back({
            .name = std::string(words[0]),
            .version = std::string(words[1]),
            .build = std::string(words[2]),
        });
    }

    return builds;
}

std::optional<std::vector<library_file_t>> interface_modules(const std::string_view text) {
    const std::vector<std::string_view> lines = lines_of(text);
    std::vector<library_file_t> modules;
    // The first line names the build, and the next the builds it's built with.
    std::size_t at = 1;
    while (at < lines.size() && lines[at].starts_with(uses_start)) {
        ++at;
    }

    while (at < lines.size()) {
        const std::string_view line = lines[at];
        const std::size_t space = line.rfind(' ');
        if (!line.starts_with(module_start) || space <= module_start.size()) {
            return std::nullopt;
        }

        std::size_t count = 0;
        const std::string_view number = line.substr(space + 1);
        const auto [end, error] =
            std::from_chars(number.data(), number.data() + number.size(), count);
        if (error != std::errc() || end != number.data() + number.size() ||
            count > lines.size() - at - 1) {
            return std::nullopt;
        }

        library_file_t module{
            .path = std::string(line.substr(module_start.size(), space - module_start.size())),
            .text = std::string(at + 1, '\n'),
        };
        if (std::ranges::contains(modules, module.path, &library_file_t::path)) {
            return std::nullopt;
        }

        for (std::size_t i = 0; i < count; ++i) {
            module.text += std::string(lines[at + 1 + i]) + "\n";
        }

        modules.push_back(std::move(module));
        at += count + 1;
    }

    return modules;
}

std::optional<std::uint32_t> source_line(const source_file_t& file, const std::uint32_t offset) {
    const std::uint32_t at = file.line_of(offset);
    for (std::uint32_t line = at + 1; line-- > 0;) {
        std::string_view text = file.line_text(line);
        text.remove_prefix(std::min(text.find_first_not_of(" \t"), text.size()));
        if (!text.starts_with(line_start)) {
            continue;
        }

        std::uint32_t first = 0;
        const std::string_view number = text.substr(line_start.size());
        const auto [end, error] =
            std::from_chars(number.data(), number.data() + number.size(), first);
        if (error == std::errc() && end == number.data() + number.size() && first > 0) {
            return first + (at - line - 1);
        }
    }

    return std::nullopt;
}

} // namespace fb
