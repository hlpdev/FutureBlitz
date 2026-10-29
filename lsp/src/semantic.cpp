#include "fb/ast.hpp"
#include "fb/build_config.hpp"
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
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace fb::lsp {
namespace {

/// The token types of the legend, in its order.
enum class token_type_t : std::uint8_t {
    NAMESPACE,
    TYPE,
    CLASS,
    ENUM,
    INTERFACE,
    STRUCT,
    TYPE_PARAMETER,
    PARAMETER,
    VARIABLE,
    PROPERTY,
    ENUM_MEMBER,
    FUNCTION,
    METHOD,
    KEYWORD,
    COMMENT,
    STRING,
    NUMBER,
};

/// Their names, in their order, separated by spaces.
constexpr std::string_view type_names = "namespace type class enum interface struct typeParameter "
                                        "parameter variable property enumMember function method "
                                        "keyword comment string number";

/// The token modifiers of the legend, as bits.
enum class modifier_t : std::uint8_t {
    DECLARATION = 1,
    READONLY = 2,
    STATIC = 4,
    DEFAULT_LIBRARY = 8,
};

constexpr std::string_view modifier_names = "declaration readonly static defaultLibrary";

/// The words of `text`, separated by spaces.
json_t words(const std::string_view text) {
    json_t out = json_t::array();
    for (std::size_t begin = 0; begin < text.size();) {
        const std::size_t end = std::min(text.find(' ', begin), text.size());
        out.push_back(std::string(text.substr(begin, end - begin)));
        begin = end + 1;
    }

    return out;
}

struct token_info_t {
    std::uint32_t end = 0;
    token_type_t type = token_type_t::VARIABLE;
    std::uint32_t modifiers = 0;
};

/// What a name of `symbol` is, as a token.
token_type_t type_of(const analysis_t& analysis, const symbol_t& symbol) {
    switch (symbol.kind) {
    case symbol_kind_t::FUNCTION:
    case symbol_kind_t::BUILTIN_FUNCTION:
        return token_type_t::FUNCTION;
    case symbol_kind_t::METHOD:
    case symbol_kind_t::CONSTRUCTOR:
        return token_type_t::METHOD;
    case symbol_kind_t::PARAMETER:
        return token_type_t::PARAMETER;
    case symbol_kind_t::FIELD:
    case symbol_kind_t::PROPERTY:
        return token_type_t::PROPERTY;
    case symbol_kind_t::MODULE:
        return token_type_t::NAMESPACE;
    case symbol_kind_t::ALIAS:
        return token_type_t::TYPE;
    case symbol_kind_t::TYPE:
        switch (analysis.types.get(symbol.type).kind) {
        case type_kind_t::CLASS:
            return token_type_t::CLASS;
        case type_kind_t::INTERFACE:
            return token_type_t::INTERFACE;
        case type_kind_t::ENUM:
            return token_type_t::ENUM;
        case type_kind_t::STRUCT:
            return token_type_t::STRUCT;
        default:
            return token_type_t::TYPE;
        }
    default:
        return token_type_t::VARIABLE;
    }
}

} // namespace

json_t semantic_tokens_legend() {
    return {
        { "tokenTypes", words(type_names) },
        { "tokenModifiers", words(modifier_names) },
    };
}

json_t semantic_tokens(const query_t& query, const positions_t& positions) {
    const source_file_t& file = query.source();
    const analysis_t& analysis = query.analysis();
    std::map<std::uint32_t, token_info_t> tokens;

    // What the lexer knows: keywords, literals, and comments.
    diagnostic_bag_t ignored;
    const std::vector<std::string_view> defined = defined_symbols(build_mode_t::DEBUG);
    for (const token_t& token : lex(file, ignored, defined)) {
        const std::string_view text = file.slice(token.span);
        std::optional<token_type_t> type;
        switch (token.kind) {
        case token_kind_t::STRING:
        case token_kind_t::STRING_START:
        case token_kind_t::STRING_MIDDLE:
        case token_kind_t::STRING_END:
        case token_kind_t::CHARACTER:
            type = token_type_t::STRING;
            break;
        case token_kind_t::INTEGER:
        case token_kind_t::DECIMAL:
            type = token_type_t::NUMBER;
            break;
        default:
            if (keyword_kind(text)) {
                type = token_type_t::KEYWORD;
            }

            break;
        }

        if (type && !token.span.empty()) {
            tokens[token.span.begin] = { .end = token.span.end, .type = *type };
        }
    }

    trivia_t trivia;
    static_cast<void>(lex_layout(file, ignored, trivia));
    for (const span_t comment : trivia.comments) {
        tokens[comment.begin] = { .end = comment.end, .type = token_type_t::COMMENT };
    }

    // An Import's dotted name, a module's.
    for (const stmt_id_t item : query.ast().items()) {
        const auto* imported = std::get_if<import_stmt_t>(&query.ast().get(item).node);
        if (imported == nullptr || imported->quoted || imported->target_span.file != file.id()) {
            continue;
        }

        std::uint32_t begin = imported->target_span.begin;
        for (std::uint32_t at = begin; at <= imported->target_span.end; ++at) {
            if (at == imported->target_span.end || file.text()[at] == '.') {
                if (at > begin) {
                    tokens[begin] = { .end = at, .type = token_type_t::NAMESPACE };
                }

                begin = at + 1;
            }
        }
    }

    // Names, as what they name, over what the lexer says of them.
    const std::string standard = file_key(std::filesystem::path(build_config::std_directory));
    std::map<file_id_t, bool> in_std;
    for (const named_t& named : query.names()) {
        const symbol_t& symbol = analysis.symbol(named.symbol);
        std::uint32_t modifiers = 0;
        if (named.declaration) {
            modifiers |= static_cast<std::uint32_t>(modifier_t::DECLARATION);
        }

        if (symbol.kind == symbol_kind_t::CONSTANT || symbol.kind == symbol_kind_t::LOOP_VARIABLE ||
            (symbol.kind == symbol_kind_t::FIELD && symbol.modifiers.is_read_only)) {
            modifiers |= static_cast<std::uint32_t>(modifier_t::READONLY);
        }

        if ((symbol.kind == symbol_kind_t::GLOBAL && symbol.owner != type_id_t::INVALID) ||
            symbol.modifiers.is_static) {
            modifiers |= static_cast<std::uint32_t>(modifier_t::STATIC);
        }

        if (symbol.kind == symbol_kind_t::BUILTIN_FUNCTION) {
            modifiers |= static_cast<std::uint32_t>(modifier_t::DEFAULT_LIBRARY);
        } else if (!symbol.span.empty()) {
            auto [found, added] = in_std.try_emplace(symbol.span.file, false);
            if (added) {
                const std::filesystem::path path = query.compilation().disk_path(symbol.span.file);
                const std::string key = path.empty() ? std::string() : file_key(path);
                found->second = key.starts_with(standard + "/");
            }

            if (found->second) {
                modifiers |= static_cast<std::uint32_t>(modifier_t::DEFAULT_LIBRARY);
            }
        }

        tokens[named.span.begin] = {
            .end = named.span.end,
            .type = type_of(analysis, symbol),
            .modifiers = modifiers,
        };
    }

    // Relative to the token before, each line of one that goes over several.
    json_t data = json_t::array();
    std::uint32_t last_line = 0;
    std::uint32_t last_column = 0;
    std::uint32_t covered = 0;
    for (const auto& [begin, token] : tokens) {
        if (begin < covered) {
            continue;
        }

        covered = token.end;
        std::uint32_t at = begin;
        while (at < token.end) {
            const location_t start = file.location_of(at, column_unit_t::UTF8);
            const std::string_view line = file.line_text(start.line);
            const std::uint32_t line_end =
                at - start.column + static_cast<std::uint32_t>(line.size());
            const std::uint32_t end = std::min(token.end, line_end);
            if (end > at) {
                const location_t position = file.location_of(at, positions.unit);
                const std::uint32_t length =
                    positions.length(file.slice({ .file = file.id(), .begin = at, .end = end }));
                data.push_back(position.line - last_line);
                data.push_back(position.line == last_line ? position.column - last_column
                                                          : position.column);
                data.push_back(length);
                data.push_back(static_cast<std::uint32_t>(token.type));
                data.push_back(token.modifiers);
                last_line = position.line;
                last_column = position.column;
            }

            // On to the next line, past its line break.
            if (start.line + 1 >= file.line_count()) {
                break;
            }

            at = file.offset_of({ .line = start.line + 1, .column = 0 }, column_unit_t::UTF8);
        }
    }

    return { { "data", std::move(data) } };
}

} // namespace fb::lsp
