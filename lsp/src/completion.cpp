#include "fb/ast.hpp"
#include "fb/diagnostic.hpp"
#include "fb/driver.hpp"
#include "fb/lexer.hpp"
#include "fb/lsp/features.hpp"
#include "fb/lsp/protocol.hpp"
#include "fb/lsp/query.hpp"
#include "fb/sema.hpp"
#include "fb/source.hpp"
#include "fb/token.hpp"
#include "fb/types.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace fb::lsp {
namespace {

/// The LSP's CompletionItemKind numbers.
enum class item_kind_t : std::uint8_t {
    METHOD = 2,
    FUNCTION = 3,
    CONSTRUCTOR = 4,
    FIELD = 5,
    VARIABLE = 6,
    CLASS = 7,
    INTERFACE = 8,
    MODULE = 9,
    PROPERTY = 10,
    ENUM = 13,
    KEYWORD = 14,
    ENUM_MEMBER = 20,
    CONSTANT = 21,
    STRUCT = 22,
    TYPE_PARAMETER = 25,
};

/// Every keyword, as the lexer spells it.
constexpr std::array keywords = {
#define FB_KEYWORD(name, spelling) std::string_view(spelling),
#include "fb/token_kinds.def"
};

/// The types the language has by name besides the numbers, Bool, String, CString, and Char,
/// and the names code writes some of those with, separated by spaces.
constexpr std::string_view built_in_types =
    "Int Long Byte Float Double Atomic Mutex Task Thread Channel Scheduler Ptr Weak";

bool is_name_char(const char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           static_cast<unsigned char>(c) >= 0x80;
}

/// `text` without the spaces and tabs at its start.
std::string_view left_trimmed(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }

    return text;
}

/// Whether `offset` is in a string's text, a quoted Import's path, a character literal, or a
/// comment, where no name goes. An interpolation's code is code.
bool in_text(const source_file_t& file, const std::uint32_t offset) {
    diagnostic_bag_t ignored;
    const std::vector<std::string_view> defined = defined_symbols(build_mode_t::DEBUG);
    for (const token_t& token : lex(file, ignored, defined)) {
        const bool literal =
            token.kind == token_kind_t::STRING || token.kind == token_kind_t::STRING_START ||
            token.kind == token_kind_t::STRING_MIDDLE || token.kind == token_kind_t::STRING_END ||
            token.kind == token_kind_t::CHARACTER;
        if (!literal || offset <= token.span.begin || offset > token.span.end) {
            continue;
        }

        // At its end, it's in only one whose quote isn't there yet, while it's typed.
        const std::string_view text = file.slice(token.span);
        const bool closed =
            text.size() > 1 && (text.ends_with('"') || text.ends_with('\'') || text.ends_with('{'));
        if (offset < token.span.end || !closed) {
            return true;
        }
    }

    trivia_t trivia;
    static_cast<void>(lex_layout(file, ignored, trivia));
    return std::ranges::any_of(trivia.comments, [&](const span_t comment) {
        const bool block = file.slice(comment).starts_with("/*");
        return offset > comment.begin && (block ? offset < comment.end : offset <= comment.end);
    });
}

/// Whether what's before the word being typed on its line, `before`, says a type goes there:
/// after a declaration's name and ':', or after As, Extends, or Implements.
bool names_type(std::string_view before) {
    while (!before.empty() && (before.back() == ' ' || before.back() == '\t')) {
        before.remove_suffix(1);
    }

    const std::size_t last = before.find_last_of(" \t(,");
    const std::string_view word = before.substr(last == std::string_view::npos ? 0 : last + 1);
    if (word == "As" || word == "Extends" || word == "Implements" ||
        (before.ends_with(',') && before.contains(" Implements "))) {
        return true;
    }

    if (!before.ends_with(':') || before.contains('?') || before.contains('=')) {
        return false;
    }

    // A declaration's, or a parameter's in a Function's or a lambda's first line, but not a
    // named argument's, whose value is code.
    // Its first word past the modifiers.
    constexpr std::string_view modifiers =
        " Public Private Package Protected Static ReadOnly ThreadLocal Override Async Unsafe "
        "Abstract Final ByRef ";
    constexpr std::string_view declarations = " Local Global Const Field Property Function Method "
                                              "Operator ";
    std::string_view rest = before;
    std::string_view first;
    while (!rest.empty()) {
        const std::size_t end = std::min(rest.find_first_of(" (<"), rest.size());
        first = rest.substr(0, end);
        if (!modifiers.contains(" " + std::string(first) + " ")) {
            break;
        }

        rest = left_trimmed(rest.substr(end));
    }

    return declarations.contains(" " + std::string(first) + " ") || rest.starts_with("New(") ||
           before.contains("Function(");
}

/// A block of statements and the part of the file it covers, which a position in it is in,
/// though the block may not reach it yet, as when a line is being typed after its last
/// statement; and the variables it has besides its own statements', like a loop's.
struct region_t {
    const block_t* block = nullptr;
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
    std::vector<symbol_id_t> variables;
};

/// Collects the completion items, each label once.
class items_t {
public:
    explicit items_t(const query_t& query) : m_query(query) {}

    void add(std::string label, const item_kind_t kind, std::string detail = {}) {
        if (label.empty() || !m_labels.insert(label).second) {
            return;
        }

        json_t item{ { "label", std::move(label) }, { "kind", static_cast<int>(kind) } };
        if (!detail.empty()) {
            item["detail"] = std::move(detail);
        }

        m_items.push_back(std::move(item));
    }

    void add(const symbol_id_t id) {
        const symbol_t& symbol = m_query.analysis().symbol(id);
        // An Operator's name isn't one code writes.
        if (symbol.name.empty() || !is_name_char(symbol.name.front()) || symbol.name == "_") {
            return;
        }

        add(std::string(symbol.name), kind_of(symbol), m_query.signature(id));
    }

    void add_raw(json_t item) {
        if (m_labels.insert(item["label"].get<std::string>()).second) {
            m_items.push_back(std::move(item));
        }
    }

    [[nodiscard]] json_t take() {
        return { { "isIncomplete", false }, { "items", std::move(m_items) } };
    }

    /// The items that name types: types, type parameters, Aliases, and modules, before a '.'.
    [[nodiscard]] json_t take_types() {
        json_t types = json_t::array();
        for (json_t& item : m_items) {
            switch (static_cast<item_kind_t>(item["kind"].get<int>())) {
            case item_kind_t::CLASS:
            case item_kind_t::INTERFACE:
            case item_kind_t::ENUM:
            case item_kind_t::STRUCT:
            case item_kind_t::MODULE:
            case item_kind_t::TYPE_PARAMETER:
                types.push_back(std::move(item));
                break;
            default:
                break;
            }
        }

        return { { "isIncomplete", false }, { "items", std::move(types) } };
    }

private:
    const query_t& m_query;
    std::unordered_set<std::string> m_labels;
    json_t m_items = json_t::array();

    [[nodiscard]] item_kind_t kind_of(const symbol_t& symbol) const {
        switch (symbol.kind) {
        case symbol_kind_t::FUNCTION:
        case symbol_kind_t::BUILTIN_FUNCTION:
            return item_kind_t::FUNCTION;
        case symbol_kind_t::METHOD:
            return item_kind_t::METHOD;
        case symbol_kind_t::CONSTRUCTOR:
            return item_kind_t::CONSTRUCTOR;
        case symbol_kind_t::FIELD:
            return item_kind_t::FIELD;
        case symbol_kind_t::PROPERTY:
            return item_kind_t::PROPERTY;
        case symbol_kind_t::CONSTANT:
            return item_kind_t::CONSTANT;
        case symbol_kind_t::MODULE:
            return item_kind_t::MODULE;
        case symbol_kind_t::ALIAS:
            return item_kind_t::TYPE_PARAMETER;
        case symbol_kind_t::TYPE:
            switch (m_query.analysis().types.get(symbol.type).kind) {
            case type_kind_t::CLASS:
                return item_kind_t::CLASS;
            case type_kind_t::INTERFACE:
                return item_kind_t::INTERFACE;
            case type_kind_t::ENUM:
                return item_kind_t::ENUM;
            default:
                return item_kind_t::STRUCT;
            }
        default:
            return item_kind_t::VARIABLE;
        }
    }
};

/// Works out the completions of one request.
class completer_t {
public:
    completer_t(const query_t& query, const std::uint32_t offset)
        : m_query(query), m_analysis(query.analysis()), m_ast(query.ast()), m_offset(offset),
          m_items(query) {
        for (const stmt_id_t item : m_ast.items()) {
            m_top_level.insert(item);
        }
    }

    /// The members of the expression before the '.' that ends at `dot`.
    json_t members(const std::uint32_t dot) {
        for (std::size_t i = 0; i < m_ast.expr_count(); ++i) {
            const expr_t& expr = m_ast.get(static_cast<expr_id_t>(i));
            const auto* member = std::get_if<member_expr_t>(&expr.node);
            if (member == nullptr || expr.span.file != m_query.file() ||
                m_ast.get(member->object).span.end != dot) {
                continue;
            }

            // A module's qualifier, a type's name, or a value.
            const expr_info_t& object = m_analysis.expr(member->object);
            const auto* name = std::get_if<name_expr_t>(&m_ast.get(member->object).node);
            const symbol_t* named = object.symbol ? &m_analysis.symbol(*object.symbol) : nullptr;
            const std::optional<type_id_t> builtin =
                name != nullptr && named == nullptr ? builtin_type(name->name) : std::nullopt;
            if (named != nullptr && named->kind == symbol_kind_t::MODULE) {
                for (const module_id_t module : named->modules) {
                    add_module_names(module, {});
                }
            } else if (named != nullptr && name != nullptr &&
                       (named->kind == symbol_kind_t::TYPE ||
                        named->kind == symbol_kind_t::ALIAS)) {
                add_statics(named->type);
            } else if (builtin) {
                add_statics(*builtin);
            } else {
                add_instance_members(object.type);
            }

            break;
        }

        return m_items.take();
    }

    /// What an Import or From can name, the word being typed starting at `begin`, and what's
    /// typed of its dotted name before it starting at `start`.
    json_t modules(const std::vector<std::string>& modules, const std::uint32_t start,
                   const positions_t& positions) {
        const std::string_view typed = m_query.source().text().substr(start, m_offset - start);
        const json_t range = positions.range(
            m_query.source(), { .file = m_query.file(), .begin = start, .end = m_offset });
        for (const std::string& module : modules) {
            if (module.starts_with(typed.substr(0, typed.rfind('.') + 1))) {
                m_items.add_raw({
                    { "label", module },
                    { "kind", static_cast<int>(item_kind_t::MODULE) },
                    { "filterText", module },
                    { "textEdit", { { "range", range }, { "newText", module } } },
                });
            }
        }

        return m_items.take();
    }

    /// The names a From line can bring in from the module `name`.
    json_t from_names(const std::string_view name) {
        const module_table_t& table = m_ast.modules();
        for (std::size_t i = 0; i < table.size(); ++i) {
            const std::string& module = table.get(static_cast<module_id_t>(i)).name;
            if (module == name || (module.starts_with(name) && module[name.size()] == '.' &&
                                   module.find('.', name.size() + 1) == std::string::npos)) {
                add_module_names(static_cast<module_id_t>(i), {});
            }
        }

        return m_items.take();
    }

    /// The names in scope at the offset, and the keywords.
    json_t names(const bool types_only) {
        add_locals();
        // The built-in types, by each name code writes them with.
        for (auto type = static_cast<std::uint32_t>(type_id_t::BOOL);
             type < static_cast<std::uint32_t>(type_id_t::BUILTIN_COUNT); ++type) {
            m_items.add(std::string(builtin_name(static_cast<type_id_t>(type))),
                        item_kind_t::STRUCT, "built-in type");
        }

        for (std::size_t begin = 0; begin < built_in_types.size();) {
            const std::size_t end =
                std::min(built_in_types.find(' ', begin), built_in_types.size());
            m_items.add(std::string(built_in_types.substr(begin, end - begin)), item_kind_t::STRUCT,
                        "built-in type");
            begin = end + 1;
        }

        if (const std::optional<module_id_t> module = m_query.module()) {
            add_own_names();
            for (const module_import_t& imported : m_ast.modules().get(*module).imports) {
                const auto* node = std::get_if<import_stmt_t>(&m_ast.get(imported.statement).node);
                if (node == nullptr || node->alias) {
                    continue;
                }

                for (const module_id_t from : imported.modules) {
                    add_module_names(from, node->from ? std::optional(&node->names) : std::nullopt);
                }
            }
        }

        // Where a type is written, only what names one.
        if (types_only) {
            return m_items.take_types();
        }

        for (std::size_t i = 0; i < m_analysis.symbols.size(); ++i) {
            if (m_analysis.symbols[i].kind == symbol_kind_t::BUILTIN_FUNCTION) {
                m_items.add(static_cast<symbol_id_t>(i));
            }
        }

        for (const std::string_view keyword : keywords) {
            m_items.add(std::string(keyword), item_kind_t::KEYWORD);
        }

        return m_items.take();
    }

private:
    const query_t& m_query;
    const analysis_t& m_analysis;
    const ast_t& m_ast;
    std::uint32_t m_offset;
    items_t m_items;
    std::unordered_set<stmt_id_t> m_top_level;

    /// Whether code in this file can use a declaration of another module's file, with its
    /// visibility.
    [[nodiscard]] bool visible(const symbol_t& symbol) const {
        if (symbol.span.file == m_query.file() || symbol.span.empty()) {
            return true;
        }

        switch (symbol.modifiers.visibility) {
        case visibility_t::PUBLIC:
            return true;
        case visibility_t::PACKAGE: {
            const std::optional<module_id_t> mine = m_query.module();
            const std::optional<module_id_t> theirs = m_ast.modules().of(symbol.span.file);
            return mine && theirs &&
                   m_ast.modules().get(*mine).package == m_ast.modules().get(*theirs).package;
        }
        case visibility_t::PROTECTED:
            return symbol.kind != symbol_kind_t::FIELD;
        case visibility_t::PRIVATE:
            return false;
        }

        return false;
    }

    /// Whether a symbol is one of a module's top-level declarations.
    [[nodiscard]] bool top_level(const symbol_t& symbol) const {
        switch (symbol.kind) {
        case symbol_kind_t::FUNCTION:
        case symbol_kind_t::TYPE:
        case symbol_kind_t::ALIAS:
            return symbol.owner == type_id_t::INVALID;
        case symbol_kind_t::GLOBAL:
        case symbol_kind_t::CONSTANT:
            return symbol.owner == type_id_t::INVALID && symbol.declaration &&
                   m_top_level.contains(*symbol.declaration);
        default:
            return false;
        }
    }

    /// A module's top-level names that this file sees, or those of `only`.
    void add_module_names(const module_id_t module,
                          const std::optional<const std::vector<name_t>*> only) {
        const file_id_t file = m_ast.modules().get(module).file;
        for (std::size_t i = 0; i < m_analysis.symbols.size(); ++i) {
            const symbol_t& symbol = m_analysis.symbols[i];
            if (symbol.span.file != file || symbol.span.empty() || !top_level(symbol) ||
                !visible(symbol)) {
                continue;
            }

            if (only && std::ranges::none_of(
                            **only, [&](const name_t& name) { return name.text == symbol.name; })) {
                continue;
            }

            m_items.add(static_cast<symbol_id_t>(i));
        }
    }

    /// This file's own top-level names, and the qualifiers of its Imports.
    void add_own_names() {
        for (std::size_t i = 0; i < m_analysis.symbols.size(); ++i) {
            const symbol_t& symbol = m_analysis.symbols[i];
            if (symbol.span.file == m_query.file() &&
                (top_level(symbol) || symbol.kind == symbol_kind_t::MODULE)) {
                m_items.add(static_cast<symbol_id_t>(i));
            }
        }
    }

    /// The types whose members a value of `type` has: it, its generic one, the Classes it
    /// extends, and the Interfaces it implements, with theirs.
    [[nodiscard]] std::set<type_id_t> owners_of(type_id_t type) const {
        const type_table_t& types = m_analysis.types;
        std::set<type_id_t> owners;
        std::vector<type_id_t> work{ type };
        while (!work.empty()) {
            const type_id_t at = work.back();
            work.pop_back();
            if (at == type_id_t::INVALID || std::to_underlying(at) >= types.size() ||
                !owners.insert(at).second) {
                continue;
            }

            const type_t& entry = types.get(at);
            work.push_back(entry.generic);
            work.push_back(entry.base);
            std::ranges::copy(entry.interfaces, std::back_inserter(work));
            // A type parameter has what its constraints' Interfaces have.
            if (entry.kind == type_kind_t::PARAMETER) {
                for (const symbol_t& symbol : m_analysis.symbols) {
                    for (const bound_t& bound : symbol.constraints) {
                        if (bound.parameter == at && bound.field == type_id_t::INVALID) {
                            work.push_back(bound.interface);
                        }
                    }
                }
            }
        }

        return owners;
    }

    /// Whether `owner` is an Extend of every array, T[], whose Methods an array of `type` has.
    [[nodiscard]] bool every_array(const type_id_t owner, const type_id_t type) const {
        const type_table_t& types = m_analysis.types;
        if (std::to_underlying(owner) >= types.size() ||
            types.get(owner).kind != type_kind_t::ARRAY) {
            return false;
        }

        const type_kind_t kind = types.get(type).kind;
        const type_id_t element = types.get(owner).inner;
        return (kind == type_kind_t::ARRAY || kind == type_kind_t::FIXED_ARRAY) &&
               element != type_id_t::INVALID && std::to_underlying(element) < types.size() &&
               types.get(element).kind == type_kind_t::PARAMETER;
    }

    /// The members a value of `type` has: its fields, Methods, and Properties, and what Extends
    /// add.
    void add_instance_members(type_id_t type) {
        const type_table_t& types = m_analysis.types;
        if (type == type_id_t::INVALID || std::to_underlying(type) >= types.size()) {
            return;
        }

        if (types.get(type).kind == type_kind_t::NULLABLE) {
            type = types.get(type).inner;
        }

        const std::set<type_id_t> owners = owners_of(type);
        for (std::size_t i = 0; i < m_analysis.symbols.size(); ++i) {
            const symbol_t& symbol = m_analysis.symbols[i];
            const bool member =
                symbol.kind == symbol_kind_t::FIELD || symbol.kind == symbol_kind_t::PROPERTY ||
                (symbol.kind == symbol_kind_t::METHOD && !symbol.modifiers.is_static);
            if (member && (owners.contains(symbol.owner) || every_array(symbol.owner, type)) &&
                visible(symbol)) {
                m_items.add(static_cast<symbol_id_t>(i));
            }
        }

        if (const auto found = m_analysis.instance_members.find(type);
            found != m_analysis.instance_members.end()) {
            for (const symbol_id_t member : found->second) {
                m_items.add(member);
            }
        }

        // What the language gives arrays and Strings.
        const type_kind_t kind = types.get(type).kind;
        if (kind == type_kind_t::ARRAY || kind == type_kind_t::FIXED_ARRAY) {
            m_items.add("Count", item_kind_t::PROPERTY, "Property Count: Int32");
        }

        if (kind == type_kind_t::ARRAY) {
            m_items.add("Capacity", item_kind_t::PROPERTY, "Property Capacity: Int32");
        }

        if (type == type_id_t::STRING) {
            m_items.add("Chars", item_kind_t::PROPERTY, "Property Chars: Char[]");
            m_items.add("Bytes", item_kind_t::PROPERTY, "Property Bytes: UInt8[]");
        }
    }

    /// What a type's name gives before a '.': an Enum's values, and its Static Fields, Methods,
    /// and Properties.
    void add_statics(const type_id_t type) {
        const type_table_t& types = m_analysis.types;
        if (type == type_id_t::INVALID || std::to_underlying(type) >= types.size()) {
            return;
        }

        for (const enum_value_t& value : types.get(type).values) {
            m_items.add(value.name, item_kind_t::ENUM_MEMBER,
                        type_name_of(type) + "." + value.name);
        }

        const std::set<type_id_t> owners = owners_of(type);
        for (std::size_t i = 0; i < m_analysis.symbols.size(); ++i) {
            const symbol_t& symbol = m_analysis.symbols[i];
            const bool member = (symbol.kind == symbol_kind_t::GLOBAL ||
                                 ((symbol.kind == symbol_kind_t::METHOD ||
                                   symbol.kind == symbol_kind_t::PROPERTY) &&
                                  symbol.modifiers.is_static));
            if (member && owners.contains(symbol.owner) && visible(symbol)) {
                m_items.add(static_cast<symbol_id_t>(i));
            }
        }
    }

    [[nodiscard]] std::string type_name_of(const type_id_t type) const {
        return m_query.type_name(type);
    }

    /// The blocks of a statement, each with the part of the file it covers.
    [[nodiscard]] std::vector<region_t> regions(const stmt_id_t id) const {
        const stmt_t& statement = m_ast.get(id);
        const stmt_info_t& info = m_analysis.stmt(id);
        const std::uint32_t end = statement.span.end;
        std::vector<region_t> out;
        // Blocks one after another, each up to where the next one's code starts.
        const auto chain = [&](std::vector<std::pair<const block_t*, std::uint32_t>> blocks,
                               std::vector<std::vector<symbol_id_t>> variables) {
            for (std::size_t i = 0; i < blocks.size(); ++i) {
                out.push_back({
                    .block = blocks[i].first,
                    .begin = blocks[i].second,
                    .end = i + 1 < blocks.size() ? blocks[i + 1].second : end,
                    .variables = i < variables.size() ? variables[i] : std::vector<symbol_id_t>{},
                });
            }
        };
        const auto begin_of = [&](const block_t& block, const std::uint32_t otherwise) {
            return block.empty() ? otherwise : m_ast.get(block.front()).span.begin;
        };
        const auto loop_variables = [&] {
            std::vector<symbol_id_t> variables;
            if (info.symbol) {
                variables.push_back(*info.symbol);
            }

            if (info.index) {
                variables.push_back(*info.index);
            }

            return variables;
        };

        std::visit(
            [&](const auto& node) {
                using node_t = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<node_t, function_stmt_t>) {
                    std::vector<symbol_id_t> variables;
                    if (info.symbol) {
                        variables = m_analysis.symbol(*info.symbol).parameters;
                        if (const std::optional<symbol_id_t> self =
                                m_analysis.symbol(*info.symbol).self) {
                            variables.push_back(*self);
                        }
                    }

                    chain({ { &node.body, std::max(node.header_end, statement.span.begin) } },
                          { variables });
                } else if constexpr (std::is_same_v<node_t, if_stmt_t>) {
                    std::vector<std::pair<const block_t*, std::uint32_t>> blocks;
                    blocks.reserve(node.branches.size() + 1);
                    for (const if_branch_t& branch : node.branches) {
                        blocks.emplace_back(&branch.body, m_ast.get(branch.condition).span.end);
                    }

                    if (node.else_body) {
                        const std::uint32_t last =
                            blocks.empty() ? statement.span.begin
                                           : begin_of(*blocks.back().first, blocks.back().second);
                        blocks.emplace_back(&*node.else_body, begin_of(*node.else_body, last));
                    }

                    chain(std::move(blocks), {});
                } else if constexpr (std::is_same_v<node_t, while_stmt_t> ||
                                     std::is_same_v<node_t, loop_stmt_t> ||
                                     std::is_same_v<node_t, unsafe_stmt_t> ||
                                     std::is_same_v<node_t, repeat_stmt_t> ||
                                     std::is_same_v<node_t, lock_stmt_t> ||
                                     std::is_same_v<node_t, for_stmt_t> ||
                                     std::is_same_v<node_t, for_in_stmt_t> ||
                                     std::is_same_v<node_t, for_member_stmt_t>) {
                    // A loop's variables, and a Lock's, which the others have none of.
                    chain({ { &node.body, statement.span.begin } }, { loop_variables() });
                } else if constexpr (std::is_same_v<node_t, select_stmt_t>) {
                    std::vector<std::pair<const block_t*, std::uint32_t>> blocks;
                    std::vector<std::vector<symbol_id_t>> variables;
                    std::size_t match = 0;
                    for (const case_t& each : node.cases) {
                        // A Case's code starts after its values, or its match's name.
                        std::uint32_t at = statement.span.begin;
                        if (!each.values.empty()) {
                            at = m_ast.get(each.values.back().value).span.end;
                        } else if (each.match) {
                            at = each.match->name.span.end;
                        }

                        blocks.emplace_back(&each.body, at);
                        if (each.match && match < info.matches.size()) {
                            variables.push_back({ info.matches[match++] });
                        } else {
                            variables.emplace_back();
                        }
                    }

                    if (node.default_body) {
                        blocks.emplace_back(
                            &*node.default_body,
                            begin_of(*node.default_body,
                                     blocks.empty() ? statement.span.begin : blocks.back().second));
                    }

                    chain(std::move(blocks), std::move(variables));
                } else if constexpr (std::is_same_v<node_t, try_stmt_t>) {
                    std::vector<std::pair<const block_t*, std::uint32_t>> blocks{
                        { &node.body, statement.span.begin },
                    };
                    std::vector<std::vector<symbol_id_t>> variables{ {} };
                    for (std::size_t i = 0; i < node.catches.size(); ++i) {
                        blocks.emplace_back(&node.catches[i].body, node.catches[i].name.span.end);
                        variables.push_back(i < info.matches.size()
                                                ? std::vector<symbol_id_t>{ info.matches[i] }
                                                : std::vector<symbol_id_t>{});
                    }

                    if (node.finally_body) {
                        blocks.emplace_back(&*node.finally_body,
                                            begin_of(*node.finally_body, blocks.back().second));
                    }

                    chain(std::move(blocks), std::move(variables));
                } else if constexpr (std::is_same_v<node_t, type_stmt_t> ||
                                     std::is_same_v<node_t, extend_stmt_t>) {
                    chain({ { &node.members, statement.span.begin } }, {});
                }
            },
            statement.node);
        return out;
    }

    /// The variables in scope at the offset: those its blocks declare before it, and its
    /// functions' parameters and loops' variables, with Self's members in a Method.
    void add_locals() {
        std::vector<symbol_id_t> found;
        walk(m_ast.items(), found);
        for (const symbol_id_t id : found | std::views::reverse) {
            m_items.add(id);
        }
    }

    void walk(const block_t& block, std::vector<symbol_id_t>& found) {
        for (const stmt_id_t id : block) {
            const stmt_t& statement = m_ast.get(id);
            if (statement.span.file != m_query.file()) {
                continue;
            }

            if (statement.span.end <= m_offset) {
                declared_by(id, found);
                continue;
            }

            if (statement.span.begin > m_offset || statement.span.end < m_offset) {
                continue;
            }

            // A Property's Get and Set are Methods of their own.
            if (const auto* property = std::get_if<property_stmt_t>(&statement.node)) {
                for (const std::optional<stmt_id_t>& part :
                     { property->getter, property->setter }) {
                    if (part) {
                        const block_t one{ *part };
                        walk(one, found);
                    }
                }

                continue;
            }

            // A generic type's type parameters, in its body.
            if (std::holds_alternative<type_stmt_t>(statement.node)) {
                for (const symbol_t& symbol : m_analysis.symbols) {
                    if (symbol.kind == symbol_kind_t::TYPE && symbol.declaration == id) {
                        add_type_parameters(m_analysis.types.get(symbol.type).parameters);
                    }
                }
            }

            for (const region_t& region : regions(id)) {
                if (region.begin <= m_offset && m_offset <= region.end) {
                    std::ranges::copy(region.variables, std::back_inserter(found));
                    add_self_members(id);
                    walk(*region.block, found);
                    break;
                }
            }
        }
    }

    /// In a Method or New, the members of Self's type, which its code uses by name.
    void add_self_members(const stmt_id_t id) {
        const std::optional<symbol_id_t> function = m_analysis.stmt(id).symbol;
        if (!std::holds_alternative<function_stmt_t>(m_ast.get(id).node) || !function) {
            return;
        }

        // A Static Method has no Self, so only its type's Statics.
        const symbol_t& symbol = m_analysis.symbol(*function);
        if (symbol.owner != type_id_t::INVALID) {
            if (!std::get<function_stmt_t>(m_ast.get(id).node).modifiers.is_static) {
                add_instance_members(symbol.owner);
            }

            add_statics(symbol.owner);
            add_type_parameters(m_analysis.types.get(symbol.owner).parameters);
        }

        add_type_parameters(symbol.type_parameters);
    }

    /// Type parameters in scope, by their names.
    void add_type_parameters(const std::vector<type_id_t>& parameters) {
        for (const type_id_t parameter : parameters) {
            if (parameter != type_id_t::INVALID &&
                m_analysis.types.get(parameter).kind == type_kind_t::PARAMETER) {
                m_items.add(std::string(m_analysis.types.name(parameter)),
                            item_kind_t::TYPE_PARAMETER);
            }
        }
    }

    /// The variables a statement before the offset declares in its block.
    void declared_by(const stmt_id_t id, std::vector<symbol_id_t>& found) const {
        // A type's Fields are its members, which only its Methods see, and a Static Method only
        // its Static ones.
        const stmt_node_t& node = m_ast.get(id).node;
        const stmt_info_t& info = m_analysis.stmt(id);
        if (const auto* variable = std::get_if<variable_stmt_t>(&node);
            variable != nullptr && variable->kind != variable_kind_t::FIELD && info.symbol) {
            found.push_back(*info.symbol);
        } else if (std::holds_alternative<destructure_stmt_t>(node)) {
            std::ranges::copy(info.matches, std::back_inserter(found));
        }
    }
};

} // namespace

std::set<std::string> visible_names(const query_t& query, const std::uint32_t offset) {
    std::set<std::string> names;
    for (const json_t& item : completer_t(query, offset).names(false)["items"]) {
        if (item["kind"].get<int>() != static_cast<int>(item_kind_t::KEYWORD)) {
            names.insert(item["label"].get<std::string>());
        }
    }

    return names;
}

json_t completion(const query_t& query, const std::uint32_t offset,
                  const std::vector<std::string>& modules, const positions_t& positions) {
    const std::string_view text = query.source().text();
    const std::uint32_t at =
        std::min<std::uint32_t>(offset, static_cast<std::uint32_t>(text.size()));
    std::uint32_t word = at;
    while (word > 0 && is_name_char(text[word - 1])) {
        --word;
    }

    // Nothing in a string's text, a quoted Import's path, a character, or a comment.
    if (in_text(query.source(), at)) {
        return { { "isIncomplete", false }, { "items", json_t::array() } };
    }

    const std::size_t line_start = word == 0 ? 0 : text.rfind('\n', word - 1) + 1;
    const std::string_view before = left_trimmed(text.substr(line_start, word - line_start));
    completer_t completer(query, at);

    // "Import std.coll", or "From std.math Import Vec3, Le".
    std::uint32_t dotted = word;
    while (dotted > 0 && (is_name_char(text[dotted - 1]) || text[dotted - 1] == '.')) {
        --dotted;
    }

    const std::string_view head = left_trimmed(text.substr(line_start, dotted - line_start));
    if (head == "Import " || head == "From " || head == "Import" || head == "From") {
        return completer.modules(modules, dotted, positions);
    }

    if (before.starts_with("From ")) {
        const std::string_view rest = left_trimmed(before.substr(5));
        const std::size_t end = rest.find(' ');
        if (end != std::string_view::npos && left_trimmed(rest.substr(end)).starts_with("Import")) {
            return completer.from_names(rest.substr(0, end));
        }
    }

    // "x." and "x?.".
    if (word > 0 && text[word - 1] == '.' && (word <= 1 || text[word - 2] != '.')) {
        const bool safe = word > 1 && text[word - 2] == '?';
        return completer.members(word - (safe ? 2 : 1));
    }

    return completer.names(names_type(before));
}

} // namespace fb::lsp
