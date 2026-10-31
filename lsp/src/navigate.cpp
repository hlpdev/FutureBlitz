#include "fb/ast.hpp"
#include "fb/diagnostic.hpp"
#include "fb/driver.hpp"
#include "fb/lexer.hpp"
#include "fb/lsp/features.hpp"
#include "fb/lsp/protocol.hpp"
#include "fb/lsp/query.hpp"
#include "fb/parser.hpp"
#include "fb/sema.hpp"
#include "fb/source.hpp"
#include "fb/token.hpp"
#include "fb/types.hpp"
#include "fb/utf8.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

// M11's step 4, the editor's basics: what folds, what a selection grows to, a name's other uses
// in its file, the hints between the code, where a value's type and an Interface's
// implementations are, and what calls what and what extends what.

namespace fb::lsp {

namespace {

/// A file parsed on its own, for what needs nothing else: its tokens, its AST, and its comments.
struct parsed_t {
    std::vector<token_t> tokens;
    ast_t ast;
    trivia_t trivia;
};

parsed_t parsed_alone(const source_file_t& file) {
    diagnostic_bag_t ignored;
    const std::vector<std::string_view> defined = defined_symbols(build_mode_t::DEBUG);
    std::vector<token_t> tokens = lex(file, ignored, defined);
    ast_t ast = parse(file, tokens, ignored);
    trivia_t trivia;
    static_cast<void>(lex_layout(file, ignored, trivia));
    return { .tokens = std::move(tokens), .ast = std::move(ast), .trivia = std::move(trivia) };
}

/// The blocks of statements a statement holds, in order.
std::vector<const block_t*> blocks_of(const stmt_node_t& node) {
    std::vector<const block_t*> found;
    const auto add = [&](const block_t& block) { found.push_back(&block); };
    if (const auto* function = std::get_if<function_stmt_t>(&node)) {
        add(function->body);
    } else if (const auto* branches = std::get_if<if_stmt_t>(&node)) {
        for (const if_branch_t& branch : branches->branches) {
            add(branch.body);
        }

        if (branches->else_body) {
            add(*branches->else_body);
        }
    } else if (const auto* loop = std::get_if<while_stmt_t>(&node)) {
        add(loop->body);
    } else if (const auto* repeat = std::get_if<repeat_stmt_t>(&node)) {
        add(repeat->body);
    } else if (const auto* forever = std::get_if<loop_stmt_t>(&node)) {
        add(forever->body);
    } else if (const auto* counted = std::get_if<for_stmt_t>(&node)) {
        add(counted->body);
    } else if (const auto* each = std::get_if<for_in_stmt_t>(&node)) {
        add(each->body);
    } else if (const auto* members = std::get_if<for_member_stmt_t>(&node)) {
        add(members->body);
    } else if (const auto* select = std::get_if<select_stmt_t>(&node)) {
        for (const case_t& option : select->cases) {
            add(option.body);
        }

        if (select->default_body) {
            add(*select->default_body);
        }
    } else if (const auto* type = std::get_if<type_stmt_t>(&node)) {
        add(type->members);
    } else if (const auto* extend = std::get_if<extend_stmt_t>(&node)) {
        add(extend->members);
    } else if (const auto* external = std::get_if<extern_stmt_t>(&node)) {
        add(external->functions);
    } else if (const auto* attempt = std::get_if<try_stmt_t>(&node)) {
        add(attempt->body);
        for (const catch_t& handler : attempt->catches) {
            add(handler.body);
        }

        if (attempt->finally_body) {
            add(*attempt->finally_body);
        }
    } else if (const auto* lock = std::get_if<lock_stmt_t>(&node)) {
        add(lock->body);
    } else if (const auto* unsafe = std::get_if<unsafe_stmt_t>(&node)) {
        add(unsafe->body);
    }

    return found;
}

/// A span from a statement's attributes and modifiers to its end.
span_t whole_of(const stmt_t& statement) {
    return {
        .file = statement.span.file,
        .begin = declaration_begin(statement),
        .end = statement.span.end,
    };
}

/// What folds in a file, each range from the line it starts on to the line before the one that
/// ends it, so the End shows, or the whole of a run of comments or of Imports.
class folds_t {
public:
    folds_t(const source_file_t& file, const parsed_t& parsed) : m_file(file), m_parsed(parsed) {}

    json_t take() {
        const ast_t& ast = m_parsed.ast;
        for (std::size_t i = 0; i < ast.stmt_count(); ++i) {
            statement(ast.get(static_cast<stmt_id_t>(i)));
        }

        for (std::size_t i = 0; i < ast.expr_count(); ++i) {
            const expr_t& expr = ast.get(static_cast<expr_id_t>(i));
            const auto* lambda = std::get_if<lambda_expr_t>(&expr.node);
            if ((lambda != nullptr && !lambda->body.empty()) ||
                std::holds_alternative<array_expr_t>(expr.node) ||
                std::holds_alternative<map_expr_t>(expr.node)) {
                until(expr.span.begin, expr.span.end);
            }
        }

        comments();
        imports();
        for (const span_t conditional : m_parsed.trivia.conditionals) {
            until(conditional.begin, conditional.end, "region");
        }

        json_t out = json_t::array();
        for (const auto& [lines, kind] : m_ranges) {
            json_t range{ { "startLine", lines.first }, { "endLine", lines.second } };
            if (!kind.empty()) {
                range["kind"] = kind;
            }

            out.push_back(std::move(range));
        }

        return out;
    }

private:
    /// From `first` to `last`, both lines shown folded as `first`.
    void add(const std::uint32_t first, const std::uint32_t last,
             const std::string_view kind = {}) {
        if (last > first) {
            m_ranges.try_emplace({ first, last }, kind);
        }
    }

    /// From the line of `begin` to the line before `end`'s.
    void until(const std::uint32_t begin, const std::uint32_t end,
               const std::string_view kind = {}) {
        const std::uint32_t last = m_file.line_of(end);
        if (last > 0) {
            add(m_file.line_of(begin), last - 1, kind);
        }
    }

    /// Where the first `word` token at or after `from`, and before `limit`, starts, or `limit`.
    [[nodiscard]] std::uint32_t keyword_at(const std::string_view word, const std::uint32_t from,
                                           const std::uint32_t limit) const {
        const std::vector<token_t>& tokens = m_parsed.tokens;
        auto token = std::ranges::lower_bound(tokens, from, {},
                                              [](const token_t& each) { return each.span.begin; });
        for (; token != tokens.end() && token->span.begin < limit; ++token) {
            if (m_file.slice(token->span) == word) {
                return token->span.begin;
            }
        }

        return limit;
    }

    /// Where a block's last statement ends, or `otherwise` for an empty one.
    [[nodiscard]] std::uint32_t end_of(const block_t& block, const std::uint32_t otherwise) const {
        return block.empty() ? otherwise : m_parsed.ast.get(block.back()).span.end;
    }

    /// Each part, from its start to the next's, the last's to `end`.
    void parts(std::vector<std::uint32_t> starts, const std::uint32_t end) {
        starts.push_back(end);
        for (std::size_t i = 0; i + 1 < starts.size(); ++i) {
            until(starts[i], starts[i + 1]);
        }
    }

    void statement(const stmt_t& statement) {
        const ast_t& ast = m_parsed.ast;
        const span_t span = statement.span;
        // An If's branches and Else fold each on its own, as a Select's Cases and a Try's Catches
        // do.
        if (const auto* node = std::get_if<if_stmt_t>(&statement.node)) {
            if (node->single_line || node->branches.empty()) {
                return;
            }

            std::vector<std::uint32_t> starts{ span.begin };
            for (std::size_t i = 1; i < node->branches.size(); ++i) {
                starts.push_back(ast.get(node->branches[i].condition).span.begin);
            }

            if (node->else_body) {
                const if_branch_t& last = node->branches.back();
                starts.push_back(keyword_at(
                    "Else", end_of(last.body, ast.get(last.condition).span.end), span.end));
            }

            parts(std::move(starts), span.end);
            return;
        }

        if (const auto* node = std::get_if<select_stmt_t>(&statement.node)) {
            until(span.begin, span.end);
            std::vector<std::uint32_t> starts;
            std::uint32_t after = ast.get(node->subject).span.end;
            for (const case_t& option : node->cases) {
                std::uint32_t begin = after;
                if (option.match) {
                    begin = option.match->name.span.begin;
                } else if (!option.values.empty()) {
                    begin = ast.get(option.values.front().value).span.begin;
                }

                starts.push_back(begin);
                after = end_of(option.body, begin);
            }

            if (node->default_body) {
                starts.push_back(keyword_at("Default", after, span.end));
            }

            parts(std::move(starts), span.end);
            return;
        }

        if (const auto* node = std::get_if<try_stmt_t>(&statement.node)) {
            std::vector<std::uint32_t> starts{ span.begin };
            std::uint32_t after = end_of(node->body, span.begin);
            for (const catch_t& handler : node->catches) {
                starts.push_back(handler.name.span.begin);
                after = end_of(handler.body, handler.name.span.end);
            }

            if (node->finally_body) {
                starts.push_back(keyword_at("Finally", after, span.end));
            }

            parts(std::move(starts), span.end);
            return;
        }

        // The rest fold whole, when they hold a block or are a declaration's.
        const bool declares = std::holds_alternative<enum_stmt_t>(statement.node) ||
                              std::holds_alternative<property_stmt_t>(statement.node);
        if (declares || !blocks_of(statement.node).empty()) {
            until(span.begin, span.end);
        }
    }

    /// Runs of line comments, each the first thing on its line, on lines that follow each other,
    /// and each comment over more than one line.
    void comments() {
        std::optional<std::pair<std::uint32_t, std::uint32_t>> run;
        const auto end_run = [&] {
            if (run) {
                add(run->first, run->second, "comment");
            }

            run.reset();
        };
        for (const span_t comment : m_parsed.trivia.comments) {
            const std::uint32_t first = m_file.line_of(comment.begin);
            const std::uint32_t last = m_file.line_of(comment.end);
            const std::string_view before = m_file.text().substr(0, comment.begin);
            const std::size_t line_start = before.rfind('\n');
            const bool alone =
                before.substr(line_start == std::string_view::npos ? 0 : line_start + 1)
                    .find_first_not_of(" \t") == std::string_view::npos;
            if (!alone) {
                end_run();
                continue;
            }

            if (last > first) {
                end_run();
                add(first, last, "comment");
                continue;
            }

            if (run && run->second + 1 == first) {
                run->second = first;
            } else {
                end_run();
                run = { first, first };
            }
        }

        end_run();
    }

    /// Each run of Import and From lines at the top level.
    void imports() {
        const ast_t& ast = m_parsed.ast;
        std::optional<span_t> run;
        for (const stmt_id_t item : ast.items()) {
            const stmt_t& statement = ast.get(item);
            if (std::holds_alternative<import_stmt_t>(statement.node)) {
                run = run ? merge(*run, statement.span) : statement.span;
                continue;
            }

            if (run) {
                add(m_file.line_of(run->begin), m_file.line_of(run->end), "imports");
                run.reset();
            }
        }

        if (run) {
            add(m_file.line_of(run->begin), m_file.line_of(run->end), "imports");
        }
    }

    const source_file_t& m_file;
    const parsed_t& m_parsed;
    /// By their lines, the first kind each has.
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::string> m_ranges;
};

/// What a name's last part is: after its last '.', without what isn't a letter, digit, or '_'.
std::string last_word(const std::string_view text) {
    std::string word;
    for (const char c :
         text.substr(text.rfind('.') == std::string_view::npos ? 0 : text.rfind('.') + 1)) {
        if (std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_') {
            word += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }

    return word;
}

std::string lowered(const std::string_view text) {
    std::string out(text);
    std::ranges::transform(
        out, out.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

/// The type a value of `type` holds, as T? and T[] hold a T, and Weak<T> a T.
type_id_t held(const type_table_t& types, type_id_t type) {
    while (type != type_id_t::INVALID && std::to_underlying(type) < types.size()) {
        const type_kind_t kind = types.get(type).kind;
        if (kind != type_kind_t::NULLABLE && kind != type_kind_t::ARRAY &&
            kind != type_kind_t::FIXED_ARRAY && kind != type_kind_t::WEAK) {
            break;
        }

        type = types.get(type).inner;
    }

    return type;
}

/// The LSP's SymbolKind of a function or a type, for a hierarchy's item.
int item_kind(const analysis_t& analysis, const symbol_t& symbol) {
    switch (symbol.kind) {
    case symbol_kind_t::METHOD:
        return 6;
    case symbol_kind_t::CONSTRUCTOR:
        return 9;
    case symbol_kind_t::PROPERTY:
        return 7;
    case symbol_kind_t::TYPE:
        if (symbol.type == type_id_t::INVALID) {
            return 5;
        }

        switch (analysis.types.get(symbol.type).kind) {
        case type_kind_t::STRUCT:
            return 23;
        case type_kind_t::INTERFACE:
            return 11;
        case type_kind_t::ENUM:
            return 10;
        default:
            return 5;
        }
    default:
        return 12;
    }
}

/// A type's name without its module's or its type arguments, as "Map" for
/// "std.collections.map.Map<String, Int>".
std::string_view bare(std::string_view name) {
    name = name.substr(0, name.find('<'));
    const std::size_t dot = name.rfind('.');
    return dot == std::string_view::npos ? name : name.substr(dot + 1);
}

/// The Interfaces a type's declaration lists, as its type has them, not those every Class has
/// without saying so.
std::vector<type_id_t> listed_interfaces(const query_t& query, const symbol_t& symbol) {
    std::vector<type_id_t> found;
    const type_table_t& types = query.analysis().types;
    if (!symbol.declaration || symbol.type == type_id_t::INVALID) {
        return found;
    }

    const auto* node = std::get_if<type_stmt_t>(&query.ast().get(*symbol.declaration).node);
    if (node == nullptr) {
        return found;
    }

    const source_file_t& file = query.compilation().sources.get(symbol.span.file);
    for (const type_expr_id_t written : node->interfaces) {
        const std::string_view name = bare(file.slice(query.ast().get(written).span));
        for (const type_id_t interface : types.get(symbol.type).interfaces) {
            if (bare(types.name(interface)) == name) {
                found.push_back(interface);
                break;
            }
        }
    }

    return found;
}

/// Whether a symbol is code a call runs: a Function, a Method, or a New, but not the one a type
/// without a New gets, which no code declares.
bool callable(const symbol_t& symbol) {
    return symbol.kind == symbol_kind_t::FUNCTION || symbol.kind == symbol_kind_t::METHOD ||
           (symbol.kind == symbol_kind_t::CONSTRUCTOR && symbol.declaration);
}

/// The Functions, Methods, and News declared in `file`, with where each is, for what's around a
/// use.
std::vector<std::pair<span_t, symbol_id_t>> functions_in(const query_t& query,
                                                         const file_id_t file) {
    std::vector<std::pair<span_t, symbol_id_t>> found;
    const ast_t& ast = query.ast();
    for (std::size_t i = 0; i < ast.stmt_count(); ++i) {
        const auto id = static_cast<stmt_id_t>(i);
        const stmt_t& statement = ast.get(id);
        if (statement.span.file != file ||
            !std::holds_alternative<function_stmt_t>(statement.node)) {
            continue;
        }

        if (const std::optional<symbol_id_t> symbol = query.analysis().stmt(id).symbol) {
            found.emplace_back(statement.span, *symbol);
        }
    }

    return found;
}

/// The innermost of `functions` around `span`.
std::optional<symbol_id_t> around(std::span<const std::pair<span_t, symbol_id_t>> functions,
                                  const span_t span) {
    std::optional<std::pair<span_t, symbol_id_t>> best;
    for (const auto& function : functions) {
        if (function.first.begin <= span.begin && span.end <= function.first.end &&
            (!best || function.first.length() < best->first.length())) {
            best = function;
        }
    }

    return best ? std::optional(best->second) : std::nullopt;
}

/// A hierarchy's item for a Function, Method, New, or type, whose data is its symbol_key_t.
std::optional<json_t> hierarchy_item(const query_t& query, const symbol_id_t id,
                                     const positions_t& positions) {
    const compilation_t& compilation = query.compilation();
    const symbol_t& symbol = query.analysis().symbol(id);
    if (symbol.span.empty()) {
        return std::nullopt;
    }

    const span_t whole =
        symbol.declaration ? whole_of(query.ast().get(*symbol.declaration)) : symbol.span;
    const std::optional<json_t> location = location_of(compilation, whole, positions);
    if (!location) {
        return std::nullopt;
    }

    const symbol_key_t key = query.key(id);
    const json_t selection =
        positions.range(compilation.sources.get(symbol.span.file), symbol.span);
    return json_t{
        { "name", std::string(symbol.name) },
        { "kind", item_kind(query.analysis(), symbol) },
        { "detail", query.signature(id) },
        { "uri", location->at("uri") },
        { "range", location->at("range") },
        { "selectionRange", selection },
        { "data", { { "file", key.file }, { "begin", key.begin }, { "name", key.name } } },
    };
}

/// A hierarchy's item for a file's top-level code, which calls as a function does.
std::optional<json_t> file_item(const compilation_t& compilation, const file_id_t file,
                                const positions_t& positions) {
    const std::filesystem::path path = compilation.disk_path(file);
    if (path.empty()) {
        return std::nullopt;
    }

    const source_file_t& source = compilation.sources.get(file);
    const span_t start{ .file = file, .begin = 0, .end = 0 };
    const span_t whole{
        .file = file,
        .begin = 0,
        .end = static_cast<std::uint32_t>(source.text().size()),
    };
    return json_t{
        { "name", to_utf8(path.filename()) },
        { "kind", 1 },
        { "uri", uri_of(path) },
        { "range", positions.range(source, whole) },
        { "selectionRange", positions.range(source, start) },
        { "data", { { "file", file_key(path) }, { "begin", 0 }, { "name", "" }, { "top", true } } },
    };
}

} // namespace

json_t folding_ranges(const source_file_t& file) {
    const parsed_t parsed = parsed_alone(file);
    return folds_t(file, parsed).take();
}

json_t selection_ranges(const source_file_t& file, const std::span<const std::uint32_t> offsets,
                        const positions_t& positions) {
    const parsed_t parsed = parsed_alone(file);
    const ast_t& ast = parsed.ast;
    // What a selection can grow to: a word, an expression, a type, a statement, a block of them,
    // and the file.
    std::vector<span_t> spans;
    spans.reserve(parsed.tokens.size() + ast.expr_count() + (3 * ast.stmt_count()));
    for (const token_t& token : parsed.tokens) {
        spans.push_back(token.span);
    }

    for (std::size_t i = 0; i < ast.expr_count(); ++i) {
        spans.push_back(ast.get(static_cast<expr_id_t>(i)).span);
    }

    for (std::size_t i = 0; i < ast.stmt_count(); ++i) {
        const stmt_t& statement = ast.get(static_cast<stmt_id_t>(i));
        spans.push_back(statement.span);
        spans.push_back(whole_of(statement));
        for (const block_t* block : blocks_of(statement.node)) {
            if (!block->empty()) {
                spans.push_back(
                    merge(whole_of(ast.get(block->front())), ast.get(block->back()).span));
            }
        }
    }

    const span_t everything{
        .file = file.id(),
        .begin = 0,
        .end = static_cast<std::uint32_t>(file.text().size()),
    };
    std::erase_if(spans, [&](const span_t span) { return span.empty() || span.file != file.id(); });
    std::ranges::sort(spans, {},
                      [](const span_t span) { return std::tuple(span.length(), span.begin); });

    json_t out = json_t::array();
    for (const std::uint32_t offset : offsets) {
        // From the innermost out, each holding the last.
        std::vector<span_t> chain{ { .file = file.id(), .begin = offset, .end = offset } };
        for (const span_t span : spans) {
            const span_t inner = chain.back();
            if (span.begin <= inner.begin && inner.end <= span.end &&
                span.length() > inner.length() && span.begin <= offset && offset <= span.end) {
                chain.push_back(span);
            }
        }

        if (chain.back().length() < everything.length()) {
            chain.push_back(everything);
        }

        // The empty one at the offset is there only to start from, unless nothing holds it.
        if (chain.size() > 1) {
            chain.erase(chain.begin());
        }

        json_t range;
        for (const span_t span : std::views::reverse(chain)) {
            json_t outer = std::move(range);
            range = { { "range", positions.range(file, span) } };
            if (!outer.is_null()) {
                range["parent"] = std::move(outer);
            }
        }

        out.push_back(std::move(range));
    }

    return out;
}

json_t document_highlights(const query_t& query, const std::uint32_t offset,
                           const positions_t& positions) {
    const std::optional<named_t> named = query.name_at(offset);
    if (!named) {
        return json_t::array();
    }

    // Where the file assigns, which writes what it names.
    const ast_t& ast = query.ast();
    std::set<std::uint32_t> written;
    const auto target = [&](const expr_id_t id) {
        const expr_t& expr = ast.get(id);
        if (std::holds_alternative<name_expr_t>(expr.node)) {
            written.insert(expr.span.begin);
        } else if (const auto* member = std::get_if<member_expr_t>(&expr.node)) {
            written.insert(member->member.span.begin);
        }
    };
    for (std::size_t i = 0; i < ast.stmt_count(); ++i) {
        const stmt_t& statement = ast.get(static_cast<stmt_id_t>(i));
        if (statement.span.file != query.file()) {
            continue;
        }

        if (const auto* assign = std::get_if<assign_stmt_t>(&statement.node)) {
            target(assign->target);
        } else if (const auto* increment = std::get_if<increment_stmt_t>(&statement.node)) {
            target(increment->target);
        }
    }

    const std::vector<symbol_id_t> same = query.symbols_of(query.key(named->symbol));
    const std::set<symbol_id_t> symbols(same.begin(), same.end());
    std::set<std::uint32_t> seen;
    json_t out = json_t::array();
    for (const named_t& each : query.names()) {
        if (!symbols.contains(each.symbol) || !seen.insert(each.span.begin).second) {
            continue;
        }

        // Write, or Read.
        const int kind = each.declaration || written.contains(each.span.begin) ? 3 : 2;
        out.push_back(
            { { "range", positions.range(query.source(), each.span) }, { "kind", kind } });
    }

    return out;
}

json_t inlay_hints(const query_t& query, const span_t range, const inlay_options_t& options,
                   const positions_t& positions) {
    const ast_t& ast = query.ast();
    const analysis_t& analysis = query.analysis();
    const source_file_t& file = query.source();
    const auto in_range = [&](const span_t span) {
        return span.file == query.file() && span.begin >= range.begin && span.begin <= range.end;
    };
    std::vector<json_t> out;
    const auto type_hint = [&](const name_t& name, const std::optional<symbol_id_t> symbol) {
        if (!symbol || name.span.empty() || name.text == "_") {
            return;
        }

        const type_id_t type = analysis.symbol(*symbol).type;
        if (type == type_id_t::INVALID || type == type_id_t::NONE) {
            return;
        }

        out.push_back({
            { "position", positions.position(file, name.span.end) },
            { "label", ": " + query.type_name(type) },
            { "kind", 1 },
        });
    };

    if (options.types) {
        for (std::size_t i = 0; i < ast.stmt_count(); ++i) {
            const auto id = static_cast<stmt_id_t>(i);
            const stmt_t& statement = ast.get(id);
            if (!in_range(statement.span)) {
                continue;
            }

            const stmt_info_t& info = analysis.stmt(id);
            if (const auto* variable = std::get_if<variable_stmt_t>(&statement.node)) {
                // A New's call says its type already.
                if (variable->kind != variable_kind_t::LOCAL || variable->type ||
                    !variable->value) {
                    continue;
                }

                const std::optional<symbol_id_t> called = analysis.expr(*variable->value).symbol;
                if (!called || analysis.symbol(*called).kind != symbol_kind_t::CONSTRUCTOR) {
                    type_hint(variable->name, info.symbol);
                }
            } else if (const auto* destructure = std::get_if<destructure_stmt_t>(&statement.node)) {
                std::size_t next = 0;
                for (const name_t& name : destructure->names) {
                    if (name.text != "_" && next < info.matches.size()) {
                        type_hint(name, info.matches[next++]);
                    }
                }
            } else if (const auto* each = std::get_if<for_in_stmt_t>(&statement.node)) {
                if (each->index) {
                    type_hint(*each->index, info.index);
                }

                type_hint(each->value, info.symbol);
            }
        }

        // A lambda's parameters without types, which take the ones the Func it's given for has.
        for (std::size_t i = 0; i < ast.expr_count(); ++i) {
            const auto id = static_cast<expr_id_t>(i);
            const expr_t& expr = ast.get(id);
            const auto* lambda = std::get_if<lambda_expr_t>(&expr.node);
            const std::optional<std::size_t> closure = analysis.expr(id).closure;
            if (lambda == nullptr || !in_range(expr.span) || !closure ||
                *closure >= analysis.closures.size()) {
                continue;
            }

            const symbol_t& function = analysis.symbol(analysis.closures[*closure].function);
            for (std::size_t k = 0; k < lambda->parameters.size() && k < function.parameters.size();
                 ++k) {
                if (!lambda->parameters[k].type) {
                    type_hint(lambda->parameters[k].name, function.parameters[k]);
                }
            }
        }
    }

    if (options.parameters) {
        for (std::size_t i = 0; i < ast.expr_count(); ++i) {
            const auto id = static_cast<expr_id_t>(i);
            const expr_t& expr = ast.get(id);
            const auto* call = std::get_if<call_expr_t>(&expr.node);
            const expr_info_t& info = analysis.expr(id);
            if (call == nullptr || !in_range(expr.span) || !info.symbol) {
                continue;
            }

            const symbol_t& function = analysis.symbol(*info.symbol);
            const std::string name = lowered(function.name);
            for (std::size_t k = 0; k < function.parameters.size() && k < info.arguments.size();
                 ++k) {
                const std::optional<expr_id_t> argument = info.arguments[k];
                const symbol_t& parameter = analysis.symbol(function.parameters[k]);
                if (!argument || parameter.variadic || parameter.name.size() < 2) {
                    continue;
                }

                // Not for a named argument, nor where the argument or the function says it, as
                // "Move(x)" for x, or "SetName(n)" for name.
                const auto written =
                    std::ranges::find(call->arguments, *argument, &argument_t::value);
                const std::string wanted = lowered(parameter.name);
                if (written == call->arguments.end() || written->name ||
                    last_word(file.slice(ast.get(*argument).span)) == wanted ||
                    (function.parameters.size() == 1 && name.ends_with(wanted))) {
                    continue;
                }

                out.push_back({
                    { "position", positions.position(file, ast.get(*argument).span.begin) },
                    { "label", std::string(parameter.name) + ":" },
                    { "kind", 2 },
                    { "paddingRight", true },
                });
            }
        }
    }

    std::ranges::stable_sort(out, {}, [](const json_t& hint) {
        return std::pair(hint["position"]["line"].get<std::uint32_t>(),
                         hint["position"]["character"].get<std::uint32_t>());
    });
    json_t hints = json_t::array();
    for (json_t& hint : out) {
        hints.push_back(std::move(hint));
    }

    return hints;
}

json_t type_definition(const query_t& query, const std::uint32_t offset,
                       const positions_t& positions) {
    const analysis_t& analysis = query.analysis();
    type_id_t type = type_id_t::INVALID;
    if (const std::optional<named_t> named = query.name_at(offset)) {
        type = analysis.symbol(named->symbol).type;
    } else if (const std::optional<expr_id_t> expr = query.expr_at(offset)) {
        type = analysis.expr(*expr).type;
    }

    const std::optional<symbol_id_t> symbol = query.type_symbol(held(analysis.types, type));
    if (!symbol || analysis.symbol(*symbol).span.empty()) {
        return nullptr;
    }

    return location_of(query.compilation(), analysis.symbol(*symbol).span, positions)
        .value_or(nullptr);
}

json_t implementations(const std::span<const std::shared_ptr<const compilation_t>> compilations,
                       const symbol_key_t& key, const positions_t& positions) {
    json_t out = json_t::array();
    std::set<std::pair<std::string, std::uint32_t>> seen;
    for (const std::shared_ptr<const compilation_t>& compilation : compilations) {
        const query_t query(*compilation, compilation->file);
        const analysis_t& analysis = compilation->analysis;
        const type_table_t& types = analysis.types;
        const std::vector<symbol_id_t> ids = query.symbols_of(key);
        const std::set<symbol_id_t> wanted(ids.begin(), ids.end());
        const auto add = [&](const symbol_t& found) {
            const std::filesystem::path path = compilation->disk_path(found.span.file);
            if (found.span.empty() || path.empty() ||
                !seen.emplace(file_key(path), found.span.begin).second) {
                return;
            }

            if (std::optional<json_t> location = location_of(*compilation, found.span, positions)) {
                out.push_back(std::move(*location));
            }
        };
        // Whether `type`'s own symbol, a generic one's for an instance, is one wanted.
        const auto is_wanted = [&](const type_id_t type) {
            const std::optional<symbol_id_t> symbol = query.type_symbol(type);
            return symbol && wanted.contains(*symbol);
        };

        // What implements or overrides the wanted members, and what overrides those.
        std::set<symbol_id_t> members;
        for (const symbol_id_t id : ids) {
            const symbol_t& symbol = analysis.symbol(id);
            if (symbol.kind == symbol_kind_t::TYPE) {
                // What implements it, and the Classes that extend those, or what extends it.
                std::vector<type_id_t> below{ symbol.type };
                for (const conformance_t& each : analysis.conformances) {
                    if (is_wanted(each.interface)) {
                        below.push_back(each.type);
                    }
                }

                for (std::size_t i = 0; i < analysis.symbols.size(); ++i) {
                    const symbol_t& other = analysis.symbols[i];
                    if (other.kind != symbol_kind_t::TYPE || other.type == type_id_t::INVALID ||
                        wanted.contains(static_cast<symbol_id_t>(i))) {
                        continue;
                    }

                    const type_kind_t kind = types.get(other.type).kind;
                    if (std::ranges::any_of(below, [&](const type_id_t type) {
                            return other.type == type || (types.get(type).kind == kind &&
                                                          types.extends(other.type, type));
                        })) {
                        add(other);
                    }
                }

                continue;
            }

            members.insert(id);
            const std::optional<symbol_id_t> owner = query.type_symbol(symbol.owner);
            if (!owner) {
                continue;
            }

            // Its place among its Interface's members, whose implementations each conformance
            // has in that place.
            const std::vector<symbol_id_t>& declared = analysis.symbol(*owner).members;
            const auto place = std::ranges::find(declared, id);
            for (const conformance_t& conformance : analysis.conformances) {
                const std::optional<symbol_id_t> interface =
                    query.type_symbol(conformance.interface);
                if (place == declared.end() || interface != owner) {
                    continue;
                }

                const auto index = static_cast<std::size_t>(place - declared.begin());
                if (index < conformance.members.size()) {
                    const symbol_id_t member = conformance.members[index];
                    if (!analysis.symbol(member).default_method) {
                        members.insert(member);
                    }
                }
            }
        }

        for (std::size_t i = 0; i < analysis.symbols.size(); ++i) {
            const symbol_t& symbol = analysis.symbols[i];
            const auto id = static_cast<symbol_id_t>(i);
            bool overrides = members.contains(id) && !wanted.contains(id);
            for (std::optional<symbol_id_t> base = symbol.overrides; base && !overrides;
                 base = analysis.symbol(*base).overrides) {
                overrides = members.contains(*base);
            }

            if (overrides) {
                add(symbol);
            }
        }
    }

    return out;
}

json_t prepare_call_hierarchy(const query_t& query, const std::uint32_t offset,
                              const positions_t& positions) {
    const std::optional<named_t> named = query.name_at(offset);
    if (!named || !callable(query.analysis().symbol(named->symbol))) {
        return nullptr;
    }

    std::optional<json_t> item = hierarchy_item(query, named->symbol, positions);
    return item ? json_t::array({ std::move(*item) }) : json_t(nullptr);
}

json_t incoming_calls(const std::span<const std::shared_ptr<const compilation_t>> compilations,
                      const symbol_key_t& key, const positions_t& positions) {
    // By the caller's key, the caller's item and where it calls.
    std::map<std::tuple<std::string, std::uint32_t, std::string>, json_t> callers;
    std::set<std::pair<std::string, std::uint32_t>> seen;
    for (const std::shared_ptr<const compilation_t>& compilation : compilations) {
        const query_t query(*compilation, compilation->file);
        const std::vector<symbol_id_t> ids = query.symbols_of(key);
        const std::set<symbol_id_t> wanted(ids.begin(), ids.end());
        std::map<file_id_t, std::vector<std::pair<span_t, symbol_id_t>>> functions;
        for (const symbol_use_t& use : compilation->analysis.uses) {
            const std::filesystem::path path = compilation->disk_path(use.span.file);
            if (!wanted.contains(use.symbol) || path.empty() ||
                !seen.emplace(file_key(path), use.span.begin).second) {
                continue;
            }

            if (!functions.contains(use.span.file)) {
                functions[use.span.file] = functions_in(query, use.span.file);
            }

            const std::optional<symbol_id_t> caller = around(functions[use.span.file], use.span);
            std::optional<json_t> item = caller ? hierarchy_item(query, *caller, positions)
                                                : file_item(*compilation, use.span.file, positions);
            if (!item) {
                continue;
            }

            const json_t& data = item->at("data");
            json_t& call =
                callers[{ data["file"].get<std::string>(), data["begin"].get<std::uint32_t>(),
                          data["name"].get<std::string>() }];
            if (call.is_null()) {
                call = { { "from", std::move(*item) }, { "fromRanges", json_t::array() } };
            }

            const std::optional<named_t> named = query.name_of(use);
            call["fromRanges"].push_back(positions.range(compilation->sources.get(use.span.file),
                                                         named ? named->span : use.span));
        }
    }

    json_t out = json_t::array();
    for (auto& [ignored, call] : callers) {
        out.push_back(std::move(call));
    }

    return out;
}

json_t outgoing_calls(const query_t& query, const symbol_key_t& key, const bool top,
                      const positions_t& positions) {
    const analysis_t& analysis = query.analysis();
    // The code it runs: its declaration's, or for a file's top-level code, the file's outside its
    // functions.
    std::optional<span_t> code;
    if (top) {
        code = span_t{
            .file = query.file(),
            .begin = 0,
            .end = static_cast<std::uint32_t>(query.source().text().size()),
        };
    } else {
        for (const symbol_id_t id : query.symbols_of(key)) {
            if (const std::optional<stmt_id_t> declaration = analysis.symbol(id).declaration) {
                code = query.ast().get(*declaration).span;
                break;
            }
        }
    }

    if (!code) {
        return json_t::array();
    }

    const std::vector<std::pair<span_t, symbol_id_t>> functions = functions_in(query, code->file);
    std::map<std::tuple<std::string, std::uint32_t, std::string>, json_t> callees;
    std::set<std::uint32_t> seen;
    for (const symbol_use_t& use : analysis.uses) {
        if (use.span.file != code->file || use.span.begin < code->begin ||
            use.span.end > code->end || !callable(analysis.symbol(use.symbol)) ||
            (top && around(functions, use.span)) || !seen.insert(use.span.begin).second) {
            continue;
        }

        std::optional<json_t> item = hierarchy_item(query, use.symbol, positions);
        if (!item) {
            continue;
        }

        const json_t& data = item->at("data");
        json_t& call =
            callees[{ data["file"].get<std::string>(), data["begin"].get<std::uint32_t>(),
                      data["name"].get<std::string>() }];
        if (call.is_null()) {
            call = { { "to", std::move(*item) }, { "fromRanges", json_t::array() } };
        }

        const std::optional<named_t> named = query.name_of(use);
        call["fromRanges"].push_back(
            positions.range(query.source(), named ? named->span : use.span));
    }

    json_t out = json_t::array();
    for (auto& [ignored, call] : callees) {
        out.push_back(std::move(call));
    }

    return out;
}

json_t prepare_type_hierarchy(const query_t& query, const std::uint32_t offset,
                              const positions_t& positions) {
    const std::optional<named_t> named = query.name_at(offset);
    if (!named || query.analysis().symbol(named->symbol).kind != symbol_kind_t::TYPE) {
        return nullptr;
    }

    std::optional<json_t> item = hierarchy_item(query, named->symbol, positions);
    return item ? json_t::array({ std::move(*item) }) : json_t(nullptr);
}

json_t supertypes(const query_t& query, const symbol_key_t& key, const positions_t& positions) {
    const analysis_t& analysis = query.analysis();
    json_t out = json_t::array();
    std::set<symbol_id_t> seen;
    for (const symbol_id_t id : query.symbols_of(key)) {
        const type_id_t type = analysis.symbol(id).type;
        if (type == type_id_t::INVALID) {
            continue;
        }

        std::vector<type_id_t> above;
        if (analysis.types.get(type).base != type_id_t::INVALID) {
            above.push_back(analysis.types.get(type).base);
        }

        std::ranges::copy(listed_interfaces(query, analysis.symbol(id)), std::back_inserter(above));
        for (const type_id_t each : above) {
            const std::optional<symbol_id_t> symbol = query.type_symbol(each);
            if (!symbol || !seen.insert(*symbol).second) {
                continue;
            }

            if (std::optional<json_t> item = hierarchy_item(query, *symbol, positions)) {
                out.push_back(std::move(*item));
            }
        }
    }

    return out;
}

json_t subtypes(const std::span<const std::shared_ptr<const compilation_t>> compilations,
                const symbol_key_t& key, const positions_t& positions) {
    json_t out = json_t::array();
    std::set<std::pair<std::string, std::uint32_t>> seen;
    for (const std::shared_ptr<const compilation_t>& compilation : compilations) {
        const query_t query(*compilation, compilation->file);
        const analysis_t& analysis = compilation->analysis;
        const std::vector<symbol_id_t> ids = query.symbols_of(key);
        const std::set<symbol_id_t> wanted(ids.begin(), ids.end());
        const auto is_wanted = [&](const type_id_t type) {
            const std::optional<symbol_id_t> symbol = query.type_symbol(type);
            return symbol && wanted.contains(*symbol);
        };
        for (std::size_t i = 0; i < analysis.symbols.size(); ++i) {
            const symbol_t& symbol = analysis.symbols[i];
            if (symbol.kind != symbol_kind_t::TYPE || symbol.type == type_id_t::INVALID ||
                symbol.span.empty() || wanted.contains(static_cast<symbol_id_t>(i))) {
                continue;
            }

            const type_t& type = analysis.types.get(symbol.type);
            if (!is_wanted(type.base) &&
                !std::ranges::any_of(listed_interfaces(query, symbol), is_wanted)) {
                continue;
            }

            const std::filesystem::path path = compilation->disk_path(symbol.span.file);
            if (path.empty() || !seen.emplace(file_key(path), symbol.span.begin).second) {
                continue;
            }

            if (std::optional<json_t> item =
                    hierarchy_item(query, static_cast<symbol_id_t>(i), positions)) {
                out.push_back(std::move(*item));
            }
        }
    }

    return out;
}

} // namespace fb::lsp
