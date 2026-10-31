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
#include "fb/utf8.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
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
    SNIPPET = 15,
    ENUM_MEMBER = 20,
    CONSTANT = 21,
    STRUCT = 22,
    TYPE_PARAMETER = 25,
};

/// Where an item sorts among the others: what the code there wants first, then what's nearest
/// it, and keywords last.
enum class rank_t : std::uint8_t {
    WANTED,
    LOCAL,
    NEAR,
    IMPORTED,
    BUILT_IN,
    UNIMPORTED,
    KEYWORD,
};

/// Where the word being typed is, which says the keywords that can go there.
enum class place_t : std::uint8_t {
    /// At the start of a line outside every declaration: declarations, and a main file's code.
    TOP_LEVEL,
    /// At the start of a line in a Struct's, Class's, Interface's, or Extend's body: members.
    MEMBERS,
    /// At the start of a line in a function's body: statements.
    STATEMENT,
    /// After the start of a line: an expression, or what goes on in one.
    EXPRESSION,
};

/// The keywords an expression can have.
constexpr std::array<std::string_view, 15> expression_keywords = {
    "True",  "False", "Null", "Self", "Super", "New",  "Function", "Async",
    "Await", "Try",   "As",   "Is",   "In",    "Then", "To",
};

/// The keywords a statement starts with, in a function's body or a main file's top level.
constexpr std::array<std::string_view, 29> statement_keywords = {
    "If",      "ElseIf", "Else",     "For",    "While",    "Repeat", "Until", "Loop",
    "Select",  "Case",   "Default",  "Return", "Local",    "Const",  "Defer", "Throw",
    "Try",     "Break",  "Continue", "Lock",   "Parallel", "Unsafe", "End",   "Catch",
    "Finally", "Import", "Self",     "Super",  "Await",
};

/// The keywords a top-level declaration starts with.
constexpr std::array<std::string_view, 18> declaration_keywords = {
    "Function", "Class",  "Struct", "Interface", "Enum",    "Alias",
    "Extend",   "Extern", "Global", "Public",    "Private", "Package",
    "Async",    "Shared", "Final",  "Abstract",  "Const",   "Import",
};

/// The keywords a member of a type starts with.
constexpr std::array<std::string_view, 18> member_keywords = {
    "Field",  "Method",   "Property", "New",       "Operator", "Override",
    "Static", "Public",   "Private",  "Protected", "Package",  "ReadOnly",
    "Final",  "Abstract", "Async",    "Const",     "Where",    "End",
};

/// The types the language has by name besides the numbers, Bool, String, CString, and Char,
/// and the names code writes some of those with, separated by spaces.
constexpr std::string_view built_in_types =
    "Int Long Byte Float Double Atomic Mutex Task Thread Channel Scheduler Ptr Weak Object";

bool is_name_char(const char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           static_cast<unsigned char>(c) >= 0x80;
}

/// `text` with each whole word that's one of `names` as its value, as a generic type's type
/// parameters become an instance's type arguments.
std::string substituted(const std::string_view text,
                        const std::map<std::string, std::string>& names) {
    std::string out;
    for (std::size_t i = 0; i < text.size();) {
        if (!is_name_char(text[i])) {
            out += text[i++];
            continue;
        }

        std::size_t end = i;
        while (end < text.size() && is_name_char(text[end])) {
            ++end;
        }

        const std::string word(text.substr(i, end - i));
        const auto found = names.find(word);
        out += found == names.end() ? word : found->second;
        i = end;
    }

    return out;
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

    /// The type the code wants where the word is, whose values rank first.
    void expect(const std::optional<type_id_t> type) {
        m_expected = type;
    }

    [[nodiscard]] std::optional<type_id_t> expected() const {
        return m_expected;
    }

    void add(std::string label, const item_kind_t kind, std::string detail = {},
             const rank_t rank = rank_t::NEAR) {
        if (label.empty() || !m_labels.insert(label).second) {
            return;
        }

        json_t item{
            { "label", label },
            { "kind", static_cast<int>(kind) },
            { "sortText", sort_text(rank, label) },
        };
        if (!detail.empty()) {
            item["detail"] = std::move(detail);
        }

        m_items.push_back(std::move(item));
    }

    void add(const symbol_id_t id, const rank_t rank = rank_t::NEAR) {
        const symbol_t& symbol = m_query.analysis().symbol(id);
        // An Operator's name isn't one code writes.
        if (symbol.name.empty() || !is_name_char(symbol.name.front()) || symbol.name == "_") {
            return;
        }

        add(std::string(symbol.name), kind_of(symbol), m_query.signature(id),
            wanted(symbol) ? rank_t::WANTED : rank);
    }

    /// An item made whole by the caller, which ranks it `rank`, unless one with its label, or
    /// `key` when it's given, is there already.
    void add_raw(json_t item, const rank_t rank = rank_t::NEAR, const std::string& key = {}) {
        const std::string label = item["label"].get<std::string>();
        if (m_labels.insert(key.empty() ? label : key).second) {
            item["sortText"] = sort_text(rank, label);
            m_items.push_back(std::move(item));
        }
    }

    /// The text an item sorts by: its rank, then its label.
    [[nodiscard]] static std::string sort_text(const rank_t rank, const std::string_view label) {
        return std::format("{}{}", static_cast<int>(rank), label);
    }

    /// Says the items are only some, so the editor asks again as the word grows.
    void incomplete() {
        m_incomplete = true;
    }

    [[nodiscard]] json_t take() {
        return { { "isIncomplete", m_incomplete }, { "items", std::move(m_items) } };
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

        return { { "isIncomplete", m_incomplete }, { "items", std::move(types) } };
    }

private:
    const query_t& m_query;
    std::unordered_set<std::string> m_labels;
    json_t m_items = json_t::array();
    std::optional<type_id_t> m_expected;
    bool m_incomplete = false;

    /// Whether a value of `symbol`, or what calling it gives, is of the type the code wants, or
    /// of the one a nullable it wants holds.
    [[nodiscard]] bool wanted(const symbol_t& symbol) const {
        switch (symbol.kind) {
        case symbol_kind_t::FUNCTION:
        case symbol_kind_t::BUILTIN_FUNCTION:
        case symbol_kind_t::METHOD:
        case symbol_kind_t::GLOBAL:
        case symbol_kind_t::CONSTANT:
        case symbol_kind_t::LOCAL:
        case symbol_kind_t::PARAMETER:
        case symbol_kind_t::LOOP_VARIABLE:
        case symbol_kind_t::FIELD:
        case symbol_kind_t::PROPERTY:
            break;
        default:
            return false;
        }

        if (!m_expected || symbol.type == type_id_t::INVALID || symbol.type == type_id_t::NONE) {
            return false;
        }

        const type_table_t& types = m_query.analysis().types;
        return symbol.type == *m_expected ||
               (types.get(*m_expected).kind == type_kind_t::NULLABLE &&
                types.get(*m_expected).inner == symbol.type);
    }

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

    /// The members of the expression before the '.' that ends at `dot`, and for a value, the
    /// postfix completions of it.
    json_t members(const std::uint32_t dot, const positions_t& positions) {
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
                add_postfix(member->object, positions);
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

    /// After '@': the attributes the language has, and the Attributes code declares, which go
    /// before a declaration.
    json_t attributes() {
        constexpr std::array<std::string_view, 8> built_in{
            "CLayout",    "Deprecated", "Flags",          "Inline",
            "MainThread", "Open",       "PanicsAtCaller", "Test",
        };
        for (const std::string_view name : built_in) {
            m_items.add(std::string(name), item_kind_t::KEYWORD, "attribute", rank_t::BUILT_IN);
        }

        for (const symbol_t& symbol : m_analysis.symbols) {
            if (symbol.kind != symbol_kind_t::TYPE || !symbol.declaration) {
                continue;
            }

            if (const auto* type = std::get_if<type_stmt_t>(&m_ast.get(*symbol.declaration).node);
                type != nullptr && type->is_attribute) {
                m_items.add(std::string(symbol.name), item_kind_t::STRUCT, "Attribute",
                            rank_t::NEAR);
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

    /// The names in scope at the offset, and the keywords that go at `place`. `before` is the
    /// line's text before the word.
    json_t names(const bool types_only, const place_t place, const std::string_view before,
                 const std::vector<export_t>& importable = {}, const positions_t& positions = {}) {
        if (!types_only) {
            m_items.expect(expected(before));
            add_wanted(before);
        }

        add_locals();
        // The built-in types, by each name code writes them with.
        for (auto type = static_cast<std::uint32_t>(type_id_t::BOOL);
             type < static_cast<std::uint32_t>(type_id_t::BUILTIN_COUNT); ++type) {
            m_items.add(std::string(builtin_name(static_cast<type_id_t>(type))),
                        item_kind_t::STRUCT, "built-in type", rank_t::BUILT_IN);
        }

        for (std::size_t begin = 0; begin < built_in_types.size();) {
            const std::size_t end =
                std::min(built_in_types.find(' ', begin), built_in_types.size());
            m_items.add(std::string(built_in_types.substr(begin, end - begin)), item_kind_t::STRUCT,
                        "built-in type", rank_t::BUILT_IN);
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
                    add_module_names(from, node->from ? std::optional(&node->names) : std::nullopt,
                                     rank_t::IMPORTED);
                }
            }
        }

        add_unimported(importable, positions);
        // Where a type is written, only what names one.
        if (types_only) {
            return m_items.take_types();
        }

        for (std::size_t i = 0; i < m_analysis.symbols.size(); ++i) {
            if (m_analysis.symbols[i].kind == symbol_kind_t::BUILTIN_FUNCTION) {
                m_items.add(static_cast<symbol_id_t>(i), rank_t::BUILT_IN);
            }
        }

        if (place == place_t::MEMBERS) {
            add_overrides();
        }

        add_keywords(place);
        return m_items.take();
    }

    /// Where the offset is, when `starts_line` says only spaces come before the word on its line:
    /// in the innermost declaration around it.
    [[nodiscard]] place_t place(const bool starts_line) const {
        if (!starts_line) {
            return place_t::EXPRESSION;
        }

        place_t found = place_t::TOP_LEVEL;
        std::uint32_t smallest = std::numeric_limits<std::uint32_t>::max();
        for (std::size_t i = 0; i < m_ast.stmt_count(); ++i) {
            const stmt_t& statement = m_ast.get(static_cast<stmt_id_t>(i));
            if (statement.span.file != m_query.file() || statement.span.begin >= m_offset ||
                m_offset > statement.span.end || statement.span.length() >= smallest) {
                continue;
            }

            if (std::holds_alternative<function_stmt_t>(statement.node)) {
                found = place_t::STATEMENT;
            } else if (std::holds_alternative<type_stmt_t>(statement.node) ||
                       std::holds_alternative<extend_stmt_t>(statement.node)) {
                found = place_t::MEMBERS;
            } else {
                continue;
            }

            smallest = statement.span.length();
        }

        return found;
    }

private:
    const query_t& m_query;
    const analysis_t& m_analysis;
    const ast_t& m_ast;
    std::uint32_t m_offset;
    items_t m_items;
    std::unordered_set<stmt_id_t> m_top_level;

    /// Whether the offset is in, or at the end of, `span` of this file.
    [[nodiscard]] bool around(const span_t span) const {
        return span.file == m_query.file() && span.begin <= m_offset && m_offset <= span.end;
    }

    /// Whether the value `id` is where the word is: around it, or missing, which the parser puts
    /// at the next token, though that's on another line.
    [[nodiscard]] bool at_value(const expr_id_t id) const {
        const span_t span = m_ast.get(id).span;
        if (around(span)) {
            return true;
        }

        if (!std::holds_alternative<error_expr_t>(m_ast.get(id).node) ||
            span.file != m_query.file() || span.begin < m_offset) {
            return false;
        }

        const std::string_view between =
            m_query.source().text().substr(m_offset, span.begin - m_offset);
        return between.find_first_not_of(" \t\r\n") == std::string_view::npos;
    }

    /// The type the code wants where the offset is, when it says: the other side of an == or !=
    /// it's on the right of, the parameter of the call's argument it's in, the variable whose
    /// written type an initializer or an assignment gives a value, or what the function a
    /// Return is in returns. `before` is the line's text before the word.
    [[nodiscard]] std::optional<type_id_t> expected(const std::string_view before) const {
        const auto known = [](const type_id_t type) -> std::optional<type_id_t> {
            if (type == type_id_t::INVALID || type == type_id_t::NONE) {
                return std::nullopt;
            }

            return type;
        };

        // The innermost comparison it's the right side of.
        std::optional<expr_id_t> compared;
        for (std::size_t i = 0; i < m_ast.expr_count(); ++i) {
            const expr_t& expr = m_ast.get(static_cast<expr_id_t>(i));
            const auto* binary = std::get_if<binary_expr_t>(&expr.node);
            if (binary != nullptr &&
                (binary->op == binary_op_t::EQUAL || binary->op == binary_op_t::NOT_EQUAL) &&
                at_value(binary->right) &&
                (!compared || expr.span.length() < m_ast.get(*compared).span.length())) {
                compared = static_cast<expr_id_t>(i);
            }
        }

        if (compared) {
            return known(
                m_analysis.expr(std::get<binary_expr_t>(m_ast.get(*compared).node).left).type);
        }

        if (const std::optional<call_at_t> call = call_at(m_query, m_offset)) {
            const symbol_t& function = m_analysis.symbol(call->candidates[call->chosen]);
            const auto& node = std::get<call_expr_t>(m_ast.get(call->call).node);
            std::optional<symbol_id_t> parameter;
            for (const argument_t& argument : node.arguments) {
                if (!argument.name || !at_value(argument.value)) {
                    continue;
                }

                const std::string_view named = argument.name->text;
                for (const symbol_id_t each : function.parameters) {
                    if (m_analysis.symbol(each).name == named) {
                        parameter = each;
                    }
                }
            }

            if (!parameter && call->active < function.parameters.size()) {
                parameter = function.parameters[call->active];
            } else if (!parameter && !function.parameters.empty() &&
                       m_analysis.symbol(function.parameters.back()).variadic) {
                parameter = function.parameters.back();
            }

            if (!parameter) {
                return std::nullopt;
            }

            const symbol_t& given = m_analysis.symbol(*parameter);
            const type_id_t type = given.type;
            if (given.variadic && type != type_id_t::INVALID &&
                m_analysis.types.get(type).kind == type_kind_t::ARRAY) {
                return known(m_analysis.types.get(type).inner);
            }

            return known(type);
        }

        // "Return" before the word: what the innermost function around it returns.
        if (left_trimmed(before) == "Return ") {
            if (const std::optional<symbol_id_t> function = innermost_function()) {
                return known(m_analysis.symbol(*function).type);
            }
        }

        for (std::size_t i = 0; i < m_ast.stmt_count(); ++i) {
            const auto id = static_cast<stmt_id_t>(i);
            const stmt_node_t& node = m_ast.get(id).node;
            const std::optional<symbol_id_t>& declares = m_analysis.stmt(id).symbol;
            if (const auto* variable = std::get_if<variable_stmt_t>(&node);
                variable != nullptr && variable->type && variable->value &&
                at_value(*variable->value) && declares) {
                return known(m_analysis.symbol(*declares).type);
            }

            if (const auto* assign = std::get_if<assign_stmt_t>(&node);
                assign != nullptr && !assign->op && at_value(assign->value)) {
                return known(m_analysis.expr(assign->target).type);
            }

            if (const auto* returned = std::get_if<return_stmt_t>(&node);
                returned != nullptr && returned->value && at_value(*returned->value)) {
                if (const std::optional<symbol_id_t> function = innermost_function()) {
                    return known(m_analysis.symbol(*function).type);
                }
            }
        }

        return std::nullopt;
    }

    /// The Function, Method, or New the offset is in, if any.
    [[nodiscard]] std::optional<symbol_id_t> innermost_function() const {
        std::optional<symbol_id_t> found;
        std::uint32_t smallest = std::numeric_limits<std::uint32_t>::max();
        for (std::size_t i = 0; i < m_ast.stmt_count(); ++i) {
            const auto id = static_cast<stmt_id_t>(i);
            const stmt_t& statement = m_ast.get(id);
            if (std::holds_alternative<function_stmt_t>(statement.node) && around(statement.span) &&
                statement.span.length() < smallest && m_analysis.stmt(id).symbol) {
                found = m_analysis.stmt(id).symbol;
                smallest = statement.span.length();
            }
        }

        return found;
    }

    /// What the code wants that isn't a name in scope: the members of an Enum it wants, and the
    /// names of the parameters of the call it starts an argument of.
    void add_wanted(const std::string_view before) {
        const std::optional<type_id_t> type = m_items.expected();
        if (type && m_analysis.types.get(*type).kind == type_kind_t::ENUM) {
            const std::string shown = m_query.type_name(*type);
            for (std::size_t i = 0; i < m_analysis.symbols.size(); ++i) {
                const auto id = static_cast<symbol_id_t>(i);
                if (m_query.enum_of(id) == *type) {
                    m_items.add(shown + "." + std::string(m_analysis.symbol(id).name),
                                item_kind_t::ENUM_MEMBER, m_query.signature(id), rank_t::WANTED);
                }
            }
        }

        // At the start of an argument: the names of the parameters the call doesn't give yet.
        const std::string_view written = before.substr(0, before.find_last_not_of(" \t") + 1);
        if (!written.ends_with('(') && !written.ends_with(',')) {
            return;
        }

        const std::optional<call_at_t> call = call_at(m_query, m_offset);
        if (!call) {
            return;
        }

        // The arguments before the word without a name give the parameters in order.
        const auto& node = std::get<call_expr_t>(m_ast.get(call->call).node);
        const auto positional = static_cast<std::size_t>(
            std::ranges::count_if(node.arguments, [&](const argument_t& each) {
                const span_t span = m_ast.get(each.value).span;
                return !each.name && span.end < m_offset && !around(span);
            }));
        const symbol_t& function = m_analysis.symbol(call->candidates[call->chosen]);
        for (std::size_t k = 0; k < function.parameters.size(); ++k) {
            const symbol_t& parameter = m_analysis.symbol(function.parameters[k]);
            const bool given =
                k < positional || std::ranges::any_of(
                                      node.arguments,
                                      [&](const argument_t& each) {
                                          return each.name && each.name->text == parameter.name;
                                      });
            if (given || parameter.name.empty()) {
                continue;
            }

            m_items.add_raw(
                {
                    { "label", std::string(parameter.name) + ":" },
                    { "kind", static_cast<int>(item_kind_t::FIELD) },
                    { "detail", m_query.type_name(parameter.type) },
                    { "insertText", std::string(parameter.name) + ": " },
                },
                rank_t::LOCAL);
        }
    }

    /// The postfix completions of the value `object`, written on one line before a '.': each
    /// writes it into what its name says, `x.if` an If on it, by its type.
    void add_postfix(const expr_id_t object, const positions_t& positions) {
        const span_t span = m_ast.get(object).span;
        const std::string_view value = m_query.source().slice(span);
        const type_id_t type = m_analysis.expr(object).type;
        if (value.contains('\n') || type == type_id_t::INVALID || type == type_id_t::NONE) {
            return;
        }

        // The value as a snippet writes it, and the indentation of its line.
        std::string escaped;
        for (const char c : value) {
            if (c == '$' || c == '}' || c == '\\') {
                escaped += '\\';
            }

            escaped += c;
        }

        const std::string_view text = m_query.source().text();
        const std::size_t line = text.rfind('\n', span.begin == 0 ? 0 : span.begin - 1);
        const std::size_t line_start =
            line == std::string_view::npos || span.begin == 0 ? 0 : line + 1;
        const std::string_view indent =
            text.substr(line_start, text.find_first_not_of(" \t", line_start) - line_start);
        const json_t range = positions.range(
            m_query.source(), { .file = m_query.file(), .begin = span.begin, .end = m_offset });
        const auto add = [&](const std::string_view label, const std::string_view detail,
                             const std::string& snippet) {
            m_items.add_raw(
                {
                    { "label", label },
                    { "kind", static_cast<int>(item_kind_t::SNIPPET) },
                    { "detail", detail },
                    { "filterText", std::string(value) + "." + std::string(label) },
                    { "insertTextFormat", 2 },
                    { "textEdit", { { "range", range }, { "newText", snippet } } },
                },
                rank_t::IMPORTED);
        };
        const std::string body = std::format("\n{}    $0\n{}", indent, indent);
        const type_t& info = m_analysis.types.get(type);
        // If, For, and Local are statements, which the value has to start.
        const bool starts = span.begin - line_start == indent.size();
        if (type == type_id_t::BOOL) {
            if (starts) {
                add("if", "If ... End If", std::format("If {}{}End If", escaped, body));
            }

            add("not", "!value", std::format("!{}", escaped));
        }

        if (info.kind == type_kind_t::NULLABLE && starts) {
            add("ifnull", "If ... == Null", std::format("If {} == Null{}End If", escaped, body));
        }

        const bool iterable = info.kind == type_kind_t::ARRAY ||
                              info.kind == type_kind_t::FIXED_ARRAY ||
                              (m_analysis.interfaces.iterable != type_id_t::INVALID &&
                               owners_of(type).contains(m_analysis.interfaces.iterable));
        if (iterable && starts) {
            add("for", "For ... In", std::format("For ${{1:each}} In {}{}End For", escaped, body));
        }

        // Try only for a call that throws.
        if (const auto* call = std::get_if<call_expr_t>(&m_ast.get(object).node);
            call != nullptr && throws(object, call->callee)) {
            add("try", "Try value", std::format("Try {}", escaped));
        }

        if (starts) {
            add("local", "Local name = value", std::format("Local ${{1:name}} = {}", escaped));
        }
    }

    /// Whether the call `call`, of `callee`, can throw: its function has a Throws, or the Func
    /// value it calls does.
    [[nodiscard]] bool throws(const expr_id_t call, const expr_id_t callee) const {
        if (const std::optional<symbol_id_t> function = m_analysis.expr(call).symbol;
            function && !m_analysis.symbol(*function).throws.empty()) {
            return true;
        }

        const type_id_t type = m_analysis.expr(callee).type;
        return type != type_id_t::INVALID &&
               m_analysis.types.get(type).kind == type_kind_t::FUNCTION &&
               !m_analysis.types.get(type).throws.empty();
    }

    /// In the body of the Struct or Class around the offset, the Methods it can write: those of
    /// the Classes it extends, to override, and those of the Interfaces it implements that it
    /// doesn't have yet, each written out with its signature.
    void add_overrides() {
        std::optional<stmt_id_t> declared;
        std::uint32_t smallest = std::numeric_limits<std::uint32_t>::max();
        for (std::size_t i = 0; i < m_ast.stmt_count(); ++i) {
            const auto id = static_cast<stmt_id_t>(i);
            const stmt_t& statement = m_ast.get(id);
            if (std::holds_alternative<type_stmt_t>(statement.node) && around(statement.span) &&
                statement.span.length() < smallest) {
                declared = id;
                smallest = statement.span.length();
            }
        }

        if (!declared) {
            return;
        }

        // A type's symbol is the one it declares.
        const stmt_id_t around_type = *declared;
        const auto symbol = std::ranges::find_if(m_analysis.symbols, [&](const symbol_t& each) {
            return each.kind == symbol_kind_t::TYPE && each.declaration == around_type;
        });
        if (symbol == m_analysis.symbols.end()) {
            return;
        }

        const type_id_t type = symbol->type;
        const type_table_t& types = m_analysis.types;
        if (type == type_id_t::INVALID || types.get(type).kind == type_kind_t::INTERFACE) {
            return;
        }

        // What it has of its own, by name and how many parameters, but for the copies of
        // Interfaces' defaults it gets.
        const auto has = [&](const symbol_t& wanted) {
            return std::ranges::any_of(m_analysis.symbols, [&](const symbol_t& own) {
                return own.owner == type && !own.default_method && own.kind == wanted.kind &&
                       own.name == wanted.name && own.parameters.size() == wanted.parameters.size();
            });
        };
        const std::string_view text = m_query.source().text();
        const std::size_t line_start = text.rfind('\n', m_offset == 0 ? 0 : m_offset - 1);
        const std::size_t begin = line_start == std::string_view::npos ? 0 : line_start + 1;
        const std::string_view indent =
            text.substr(begin, text.find_first_not_of(" \t", begin) - begin);
        // An instance's type arguments, by the names of its generic type's type parameters.
        const auto arguments_of = [&](const type_id_t instance) {
            std::map<std::string, std::string> names;
            const type_t& info = types.get(instance);
            if (info.generic == type_id_t::INVALID) {
                return names;
            }

            const std::vector<type_id_t>& parameters = types.get(info.generic).parameters;
            for (std::size_t i = 0; i < parameters.size() && i < info.arguments.size(); ++i) {
                names.emplace(types.name(parameters[i]), types.name(info.arguments[i]));
            }

            return names;
        };
        const auto add = [&](const symbol_id_t id, const bool overriding,
                             const std::map<std::string, std::string>& arguments) {
            const symbol_t& method = m_analysis.symbol(id);
            if (method.kind != symbol_kind_t::METHOD || method.modifiers.is_static ||
                method.modifiers.is_final || method.name == "Destroy" || has(method)) {
                return;
            }

            // As a declaration writes it, without the type it's a member of before its name, and
            // with the instance's type arguments for its type's parameters.
            std::string signature = m_query.signature(id);
            const std::string owner = m_query.type_name(method.owner) + ".";
            if (const std::size_t at = signature.find(owner + std::string(method.name) + "(");
                at != std::string::npos) {
                signature.erase(at, owner.size());
            }

            signature = substituted(signature, arguments);

            const std::string head = (overriding ? "Override " : "") + signature;
            const std::string written =
                std::format("{}\n{}    $0\n{}End Method", head, indent, indent);
            m_items.add_raw(
                {
                    { "label", head },
                    { "kind", static_cast<int>(item_kind_t::METHOD) },
                    { "filterText", std::string(method.name) },
                    { "insertTextFormat", 2 },
                    { "insertText", written },
                },
                rank_t::WANTED);
        };

        // The Interfaces its declaration lists, not those every Class has without saying so, nor
        // those the Classes it extends implement already.
        std::vector<type_id_t> interfaces;
        for (const type_expr_id_t written :
             std::get<type_stmt_t>(m_ast.get(around_type).node).interfaces) {
            std::string_view name = m_query.source().slice(m_ast.get(written).span);
            name = name.substr(0, name.find('<'));
            name = name.substr(name.rfind('.') == std::string_view::npos ? 0 : name.rfind('.') + 1);
            for (const type_id_t interface : types.get(type).interfaces) {
                std::string_view own = types.name(interface);
                own = own.substr(0, own.find('<'));
                if (own == name) {
                    interfaces.push_back(interface);
                }
            }
        }

        std::set<type_id_t> seen;
        for (type_id_t base = types.get(type).base; base != type_id_t::INVALID;
             base = types.get(base).base) {
            if (!seen.insert(base).second) {
                break;
            }

            const std::map<std::string, std::string> arguments = arguments_of(base);
            for (std::size_t i = 0; i < m_analysis.symbols.size(); ++i) {
                if (m_analysis.symbols[i].owner == base ||
                    m_analysis.symbols[i].owner == types.get(base).generic) {
                    add(static_cast<symbol_id_t>(i), true, arguments);
                }
            }
        }

        // An Interface's Methods, and those of the Interfaces it extends: one without a default
        // is implemented, and one with one written again, neither with Override, which is only
        // for a Class's.
        while (!interfaces.empty()) {
            const type_id_t interface = interfaces.back();
            interfaces.pop_back();
            if (interface == type_id_t::INVALID || !seen.insert(interface).second) {
                continue;
            }

            std::ranges::copy(types.get(interface).interfaces, std::back_inserter(interfaces));
            const type_id_t generic = types.get(interface).generic;
            const std::map<std::string, std::string> arguments = arguments_of(interface);
            for (std::size_t i = 0; i < m_analysis.symbols.size(); ++i) {
                const symbol_t& member = m_analysis.symbols[i];
                if (member.owner == interface ||
                    (generic != type_id_t::INVALID && member.owner == generic)) {
                    add(static_cast<symbol_id_t>(i), false, arguments);
                }
            }
        }
    }

    /// The names of modules the file doesn't import, after the rest, each with the Import its
    /// item adds after the file's Imports.
    void add_unimported(const std::vector<export_t>& importable, const positions_t& positions) {
        const std::optional<module_id_t> own = m_query.module();
        if (!own) {
            return;
        }

        // The modules it imports, as an Import names them, and its own.
        const module_table_t& table = m_ast.modules();
        std::set<std::string> imported{ table.get(*own).name };
        for (const module_import_t& each : table.get(*own).imports) {
            if (const auto* node = std::get_if<import_stmt_t>(&m_ast.get(each.statement).node)) {
                imported.insert(node->target);
            }

            for (const module_id_t module : each.modules) {
                imported.insert(table.get(module).name);
            }
        }

        // After the Imports at its top, as one after its code is an error, or at its start.
        std::uint32_t at = 0;
        const std::string_view text = m_query.source().text();
        for (const stmt_id_t item : m_ast.items()) {
            const stmt_t& statement = m_ast.get(item);
            if (statement.span.file != m_query.file()) {
                continue;
            }

            if (!std::holds_alternative<import_stmt_t>(statement.node)) {
                break;
            }

            const std::size_t line_end = text.find('\n', statement.span.end);
            at = line_end == std::string_view::npos ? static_cast<std::uint32_t>(text.size())
                                                    : static_cast<std::uint32_t>(line_end + 1);
        }

        // Only those that start as the word being typed does, as there are many, and the editor
        // asks again as it's typed further.
        std::uint32_t word = m_offset;
        while (word > 0 && is_name_char(text[word - 1])) {
            --word;
        }

        const std::string_view typed = text.substr(word, m_offset - word);
        if (typed.empty()) {
            return;
        }

        m_items.incomplete();
        const json_t range =
            positions.range(m_query.source(), { .file = m_query.file(), .begin = at, .end = at });
        const std::filesystem::path here = m_query.compilation().disk_path(m_query.file());
        for (const export_t& name : importable) {
            if (name.name.size() < typed.size() ||
                !std::ranges::equal(std::string_view(name.name).substr(0, typed.size()), typed,
                                    [](const char a, const char b) {
                                        return std::tolower(static_cast<unsigned char>(a)) ==
                                               std::tolower(static_cast<unsigned char>(b));
                                    })) {
                continue;
            }

            // std/core/'s are every file's.
            if (imported.contains(name.module) || name.module.starts_with("std.core") ||
                name.module == "std.collections.map") {
                continue;
            }

            // A .proto's module, and a program's without a project.toml, by its path from here.
            std::string target = name.module;
            if (!name.quoted.empty() && !here.empty()) {
                std::filesystem::path relative = name.quoted.lexically_relative(here.parent_path());
                if (relative.extension() == ".fb") {
                    relative.replace_extension();
                }

                std::string path = to_utf8(relative);
                std::ranges::replace(path, '\\', '/');
                const std::string written = (path.starts_with("../") ? "" : "./") + path;
                if (relative.empty() || imported.contains(written)) {
                    continue;
                }

                target = std::format("\"{}\"", written);
            }

            const json_t import_edit{
                { "range", range },
                { "newText", std::format("Import {}\n", target) },
            };
            m_items.add_raw(
                {
                    { "label", name.name },
                    { "kind", static_cast<int>(item_kind_of(name.kind)) },
                    { "detail", std::format("{} from {}", name.kind, target) },
                    { "additionalTextEdits", json_t::array({ import_edit }) },
                },
                rank_t::UNIMPORTED, std::format("{} from {}", name.name, target));
        }
    }

    /// The completion item kind of a declaration's keyword.
    [[nodiscard]] static item_kind_t item_kind_of(const std::string_view kind) {
        if (kind == "Class") {
            return item_kind_t::CLASS;
        }

        if (kind == "Interface") {
            return item_kind_t::INTERFACE;
        }

        if (kind == "Enum") {
            return item_kind_t::ENUM;
        }

        if (kind == "Struct" || kind == "Attribute") {
            return item_kind_t::STRUCT;
        }

        if (kind == "Alias") {
            return item_kind_t::TYPE_PARAMETER;
        }

        if (kind == "Const") {
            return item_kind_t::CONSTANT;
        }

        if (kind == "Global") {
            return item_kind_t::VARIABLE;
        }

        return item_kind_t::FUNCTION;
    }

    /// The keywords that go at `place`, a value the code wants first.
    void add_keywords(const place_t place) {
        const auto add = [&](const std::string_view keyword) {
            const std::optional<type_id_t> type = m_items.expected();
            bool wanted = false;
            if (type && (keyword == "True" || keyword == "False")) {
                wanted = *type == type_id_t::BOOL;
            } else if (type && keyword == "Null") {
                wanted = m_analysis.types.get(*type).kind == type_kind_t::NULLABLE;
            }

            m_items.add(std::string(keyword), item_kind_t::KEYWORD, {},
                        wanted ? rank_t::WANTED : rank_t::KEYWORD);
        };
        switch (place) {
        case place_t::TOP_LEVEL:
            std::ranges::for_each(declaration_keywords, add);
            std::ranges::for_each(statement_keywords, add);
            break;
        case place_t::MEMBERS:
            std::ranges::for_each(member_keywords, add);
            break;
        case place_t::STATEMENT:
            std::ranges::for_each(statement_keywords, add);
            break;
        case place_t::EXPRESSION:
            std::ranges::for_each(expression_keywords, add);
            break;
        }
    }

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
                   m_ast.modules().sees_package(m_ast.modules().get(*mine).package,
                                                m_ast.modules().get(*theirs).package);
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
                          const std::optional<const std::vector<name_t>*> only,
                          const rank_t rank = rank_t::NEAR) {
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

            m_items.add(static_cast<symbol_id_t>(i), rank);
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
            m_items.add(id, rank_t::LOCAL);
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
    // Held, since a range-for keeps the temporaries of its range only from C++23's P2718 on,
    // which GCC 14 doesn't have: there, the items were read after they were freed.
    const json_t found = completer_t(query, offset).names(false, place_t::EXPRESSION, {});
    for (const json_t& item : found["items"]) {
        if (item["kind"].get<int>() != static_cast<int>(item_kind_t::KEYWORD)) {
            names.insert(item["label"].get<std::string>());
        }
    }

    return names;
}

json_t completion(const query_t& query, const std::uint32_t offset,
                  const std::function<std::vector<std::string>()>& modules,
                  const std::function<std::vector<export_t>()>& importable,
                  const positions_t& positions) {
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
        return completer.modules(modules(), dotted, positions);
    }

    if (before.starts_with("From ")) {
        const std::string_view rest = left_trimmed(before.substr(5));
        const std::size_t end = rest.find(' ');
        if (end != std::string_view::npos && left_trimmed(rest.substr(end)).starts_with("Import")) {
            return completer.from_names(rest.substr(0, end));
        }
    }

    // "@Ma", an attribute.
    if (word > 0 && text[word - 1] == '@') {
        return completer.attributes();
    }

    // "x." and "x?.".
    if (word > 0 && text[word - 1] == '.' && (word <= 1 || text[word - 2] != '.')) {
        const bool safe = word > 1 && text[word - 2] == '?';
        return completer.members(word - (safe ? 2 : 1), positions);
    }

    return completer.names(names_type(before), completer.place(before.empty()), before,
                           importable(), positions);
}

} // namespace fb::lsp
