#include "fb/proto.hpp"

#include "fb/token.hpp"
#include "fb/types.hpp"
#include "fb/utf8.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <fast_float/fast_float.h>
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
#include <utility>
#include <vector>

namespace fb {
namespace {

/// The highest field number: a tag keeps 3 bits for the wire type.
constexpr std::int32_t max_field_number = 536870911;

/// Field numbers protobuf keeps for its own implementations.
constexpr std::int32_t first_reserved_number = 19000;
constexpr std::int32_t last_reserved_number = 19999;

enum class lexeme_t : std::uint8_t {
    IDENTIFIER,
    INTEGER,
    FLOAT,
    STRING,
    /// One character of punctuation, like ';' or '{'.
    SYMBOL,
    END,
};

struct proto_token_t {
    lexeme_t kind = lexeme_t::END;
    std::string_view text;
    span_t span;
    /// A string's bytes, its escapes undone.
    std::string value;
    std::uint64_t integer = 0;
    double number = 0;
    /// A number that's wrong, which is reported already, so what it's in is skipped.
    bool wrong = false;
};

/// Whether `c` starts an identifier.
bool is_letter(const char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool is_digit(const char c) {
    return c >= '0' && c <= '9';
}

bool is_hex_digit(const char c) {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int hex_value(const char c) {
    if (is_digit(c)) {
        return c - '0';
    }

    return (c >= 'a' && c <= 'f') ? c - 'a' + 10 : c - 'A' + 10;
}

/// Whether text is well-formed UTF-8, as a String is.
bool is_utf8(const std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const decoded_t decoded = decode_utf8(text, i);
        if (!decoded.valid()) {
            return false;
        }

        i += decoded.length;
    }

    return true;
}

/// Splits a .proto's text into tokens, leaving out whitespace and comments, and reports what
/// isn't one.
class proto_lexer_t {
public:
    proto_lexer_t(const source_file_t& file, diagnostic_bag_t& diagnostics)
        : m_file(file), m_text(file.text()), m_diagnostics(diagnostics) {}

    std::vector<proto_token_t> run() {
        std::vector<proto_token_t> tokens;
        while (true) {
            skip_space();
            if (m_at >= m_text.size()) {
                tokens.push_back({ .kind = lexeme_t::END, .span = span(m_at, m_at) });
                return tokens;
            }

            tokens.push_back(next());
        }
    }

private:
    const source_file_t& m_file;
    std::string_view m_text;
    diagnostic_bag_t& m_diagnostics;
    std::size_t m_at = 0;

    [[nodiscard]] span_t span(const std::size_t begin, const std::size_t end) const {
        return {
            .file = m_file.id(),
            .begin = static_cast<std::uint32_t>(begin),
            .end = static_cast<std::uint32_t>(end),
        };
    }

    [[nodiscard]] char at(const std::size_t offset = 0) const {
        return m_at + offset < m_text.size() ? m_text[m_at + offset] : '\0';
    }

    /// Whitespace, `// ...` to the end of the line, and `/* ... */`, which doesn't nest.
    void skip_space() {
        while (m_at < m_text.size()) {
            const char c = m_text[m_at];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
                ++m_at;
            } else if (c == '/' && at(1) == '/') {
                while (m_at < m_text.size() && m_text[m_at] != '\n') {
                    ++m_at;
                }
            } else if (c == '/' && at(1) == '*') {
                const std::size_t begin = m_at;
                const std::size_t end = m_text.find("*/", m_at + 2);
                if (end == std::string_view::npos) {
                    m_diagnostics.error(span(begin, begin + 2), "a /* comment needs its */");
                    m_at = m_text.size();
                } else {
                    m_at = end + 2;
                }
            } else {
                return;
            }
        }
    }

    proto_token_t next() {
        const std::size_t begin = m_at;
        const char c = m_text[m_at];
        if (is_letter(c)) {
            while (is_letter(at()) || is_digit(at())) {
                ++m_at;
            }

            return { .kind = lexeme_t::IDENTIFIER,
                     .text = slice(begin),
                     .span = span(begin, m_at) };
        }

        if (is_digit(c) || (c == '.' && is_digit(at(1)))) {
            return number();
        }

        if (c == '"' || c == '\'') {
            return string();
        }

        // A character that's no token, as part of a UTF-8 one too.
        if (static_cast<unsigned char>(c) >= 0x80 ||
            std::string_view("{}[]()<>;=,.-+:/").find(c) == std::string_view::npos) {
            const std::size_t length = static_cast<unsigned char>(c) >= 0x80
                                           ? std::max<std::size_t>(utf8_sequence_length(c), 1)
                                           : 1;
            m_at = std::min(m_at + length, m_text.size());
            m_diagnostics.error(span(begin, m_at), "'{}' isn't part of a .proto's syntax",
                                slice(begin));
            return next_or_end();
        }

        ++m_at;
        return { .kind = lexeme_t::SYMBOL, .text = slice(begin), .span = span(begin, m_at) };
    }

    /// The token after a character that isn't one.
    proto_token_t next_or_end() {
        skip_space();
        if (m_at >= m_text.size()) {
            return { .kind = lexeme_t::END, .span = span(m_at, m_at) };
        }

        return next();
    }

    [[nodiscard]] std::string_view slice(const std::size_t begin) const {
        return m_text.substr(begin, m_at - begin);
    }

    /// How many bytes a UTF-8 character that starts with `lead` takes.
    static std::size_t utf8_sequence_length(const char lead) {
        const auto byte = static_cast<unsigned char>(lead);
        if (byte >= 0xF0) {
            return 4;
        }

        return byte >= 0xE0 ? 3 : 2;
    }

    /// An integer, decimal, octal after a '0', or hex after "0x", or a float, with a fraction or
    /// an exponent.
    proto_token_t number() {
        const std::size_t begin = m_at;
        if (at() == '0' && (at(1) == 'x' || at(1) == 'X')) {
            m_at += 2;
            std::uint64_t value = 0;
            bool overflow = false;
            const std::size_t digits = m_at;
            while (is_hex_digit(at())) {
                overflow = overflow || value > (std::numeric_limits<std::uint64_t>::max() >> 4);
                value = (value << 4) | static_cast<std::uint64_t>(hex_value(at()));
                ++m_at;
            }

            const bool empty = m_at == digits;
            if (empty) {
                m_diagnostics.error(span(begin, m_at), "a hex number needs digits after 0x");
            }

            proto_token_t token = finish_integer(begin, value, overflow);
            token.wrong = token.wrong || empty;
            return token;
        }

        while (is_digit(at())) {
            ++m_at;
        }

        bool real = false;
        if (at() == '.') {
            real = true;
            ++m_at;
            while (is_digit(at())) {
                ++m_at;
            }
        }

        if (at() == 'e' || at() == 'E') {
            const std::size_t mark = m_at;
            ++m_at;
            if (at() == '+' || at() == '-') {
                ++m_at;
            }

            if (is_digit(at())) {
                real = true;
                while (is_digit(at())) {
                    ++m_at;
                }
            } else {
                m_at = mark;
            }
        }

        if (real) {
            double value = 0;
            const std::string_view text = slice(begin);
            // fast_float doesn't read a '.' first, or a fraction without digits after the dot.
            std::string written(text);
            if (written.front() == '.') {
                written.insert(written.begin(), '0');
            }

            (void)fast_float::from_chars(written.data(), written.data() + written.size(), value);
            const bool wrong = !check_after(begin);
            return {
                .kind = lexeme_t::FLOAT,
                .text = slice(begin),
                .span = span(begin, m_at),
                .number = value,
                .wrong = wrong,
            };
        }

        const std::string_view digits = slice(begin);
        const bool octal = digits.size() > 1 && digits.front() == '0';
        const std::uint64_t base = octal ? 8 : 10;
        std::uint64_t value = 0;
        bool overflow = false;
        for (const char digit : digits) {
            if (octal && digit > '7') {
                m_diagnostics.error(span(begin, m_at),
                                    "{} isn't an octal number, which has only the digits 0 to 7",
                                    digits);
                proto_token_t token = finish_integer(begin, 0, false);
                token.wrong = true;
                return token;
            }

            const auto amount = static_cast<std::uint64_t>(digit - '0');
            overflow =
                overflow || value > (std::numeric_limits<std::uint64_t>::max() - amount) / base;
            value = value * base + amount;
        }

        return finish_integer(begin, value, overflow);
    }

    proto_token_t finish_integer(const std::size_t begin, const std::uint64_t value,
                                 const bool overflow) {
        if (overflow) {
            m_diagnostics.error(span(begin, m_at), "{} doesn't fit in 64 bits", slice(begin));
        }

        const bool wrong = !check_after(begin) || overflow;
        return {
            .kind = lexeme_t::INTEGER,
            .text = slice(begin),
            .span = span(begin, m_at),
            .integer = value,
            .wrong = wrong,
        };
    }

    /// A number can't run into a name: "12abc". False when it does.
    bool check_after(const std::size_t begin) {
        if (!is_letter(at())) {
            return true;
        }

        while (is_letter(at()) || is_digit(at())) {
            ++m_at;
        }

        m_diagnostics.error(span(begin, m_at), "'{}' is a number with letters after it",
                            slice(begin));
        return false;
    }

    /// A string in quotes, on one line, with C's escapes and protobuf's \u and \U.
    proto_token_t string() {
        const std::size_t begin = m_at;
        const char quote = m_text[m_at++];
        std::string value;
        while (true) {
            if (m_at >= m_text.size() || at() == '\n' || at() == '\r') {
                m_diagnostics.error(span(begin, m_at), "a string needs its closing {} on its line",
                                    quote);
                break;
            }

            const char c = m_text[m_at];
            if (c == quote) {
                ++m_at;
                break;
            }

            if (c != '\\') {
                value += c;
                ++m_at;
                continue;
            }

            escape(value);
        }

        return {
            .kind = lexeme_t::STRING,
            .text = slice(begin),
            .span = span(begin, m_at),
            .value = std::move(value),
        };
    }

    /// An escape in a string, from its backslash, into `value`.
    void escape(std::string& value) {
        const std::size_t begin = m_at;
        ++m_at;
        const char c = at();
        ++m_at;
        switch (c) {
        case 'a':
            value += '\a';
            return;
        case 'b':
            value += '\b';
            return;
        case 'f':
            value += '\f';
            return;
        case 'n':
            value += '\n';
            return;
        case 'r':
            value += '\r';
            return;
        case 't':
            value += '\t';
            return;
        case 'v':
            value += '\v';
            return;
        case '\\':
        case '\'':
        case '"':
        case '?':
            value += c;
            return;
        case 'x':
        case 'X': {
            int byte = 0;
            int digits = 0;
            while (digits < 2 && is_hex_digit(at())) {
                byte = byte * 16 + hex_value(at());
                ++m_at;
                ++digits;
            }

            if (digits == 0) {
                m_diagnostics.error(span(begin, m_at), "\\x needs a hex digit after it");
            }

            value += static_cast<char>(byte);
            return;
        }
        case 'u':
        case 'U': {
            const int count = c == 'u' ? 4 : 8;
            std::uint32_t point = 0;
            for (int i = 0; i < count; ++i) {
                if (!is_hex_digit(at())) {
                    m_diagnostics.error(span(begin, m_at), "\\{} needs {} hex digits after it", c,
                                        count);
                    return;
                }

                point = point * 16 + static_cast<std::uint32_t>(hex_value(at()));
                ++m_at;
            }

            if (point > 0x10FFFF || (point >= 0xD800 && point <= 0xDFFF)) {
                m_diagnostics.error(span(begin, m_at), "'{}' isn't a character's code point",
                                    m_text.substr(begin, m_at - begin));
                return;
            }

            append_utf8(value, static_cast<char32_t>(point));
            return;
        }
        default:
            break;
        }

        if (c >= '0' && c <= '7') {
            int byte = c - '0';
            for (int digits = 1; digits < 3 && at() >= '0' && at() <= '7'; ++digits) {
                byte = byte * 8 + (at() - '0');
                ++m_at;
            }

            if (byte > 0xFF) {
                m_diagnostics.error(span(begin, m_at), "'{}' is more than a byte holds",
                                    m_text.substr(begin, m_at - begin));
            }

            value += static_cast<char>(byte);
            return;
        }

        m_at = std::min(m_at, m_text.size());
        m_diagnostics.error(span(begin, m_at), "'{}' isn't an escape a .proto's string has",
                            m_text.substr(begin, m_at - begin));
    }
};

/// The scalar type a name is, if it's one.
std::optional<proto_kind_t> scalar_kind(const std::string_view name) {
    static constexpr std::array<std::pair<std::string_view, proto_kind_t>, 15> scalars{ {
        { "double", proto_kind_t::DOUBLE },
        { "float", proto_kind_t::FLOAT },
        { "int32", proto_kind_t::INT32 },
        { "int64", proto_kind_t::INT64 },
        { "uint32", proto_kind_t::UINT32 },
        { "uint64", proto_kind_t::UINT64 },
        { "sint32", proto_kind_t::SINT32 },
        { "sint64", proto_kind_t::SINT64 },
        { "fixed32", proto_kind_t::FIXED32 },
        { "fixed64", proto_kind_t::FIXED64 },
        { "sfixed32", proto_kind_t::SFIXED32 },
        { "sfixed64", proto_kind_t::SFIXED64 },
        { "bool", proto_kind_t::BOOL },
        { "string", proto_kind_t::STRING },
        { "bytes", proto_kind_t::BYTES },
    } };
    for (const auto& [written, kind] : scalars) {
        if (written == name) {
            return kind;
        }
    }

    return std::nullopt;
}

/// A scalar type's name, as a .proto writes it.
std::string_view scalar_name(const proto_kind_t kind) {
    switch (kind) {
    case proto_kind_t::DOUBLE:
        return "double";
    case proto_kind_t::FLOAT:
        return "float";
    case proto_kind_t::INT32:
        return "int32";
    case proto_kind_t::INT64:
        return "int64";
    case proto_kind_t::UINT32:
        return "uint32";
    case proto_kind_t::UINT64:
        return "uint64";
    case proto_kind_t::SINT32:
        return "sint32";
    case proto_kind_t::SINT64:
        return "sint64";
    case proto_kind_t::FIXED32:
        return "fixed32";
    case proto_kind_t::FIXED64:
        return "fixed64";
    case proto_kind_t::SFIXED32:
        return "sfixed32";
    case proto_kind_t::SFIXED64:
        return "sfixed64";
    case proto_kind_t::BOOL:
        return "bool";
    case proto_kind_t::STRING:
        return "string";
    case proto_kind_t::BYTES:
        return "bytes";
    case proto_kind_t::NAMED:
    case proto_kind_t::MESSAGE:
    case proto_kind_t::ENUM:
        break;
    }

    return "";
}

/// Whether a repeated field of this kind can be packed: a number, a bool, or an enum.
bool is_packable(const proto_kind_t kind) {
    return kind != proto_kind_t::STRING && kind != proto_kind_t::BYTES &&
           kind != proto_kind_t::MESSAGE && kind != proto_kind_t::NAMED;
}

/// The options protobuf's descriptor.proto gives each kind of declaration, which a .proto sets by
/// their names. One in parentheses is a custom option, which the file declares with an extend,
/// and doesn't change what fb makes.
const std::vector<std::string_view>& known_options(const std::string_view where) {
    static const std::vector<std::string_view> file{
        "java_package",
        "java_outer_classname",
        "java_multiple_files",
        "java_generate_equals_and_hash",
        "java_string_check_utf8",
        "optimize_for",
        "go_package",
        "cc_generic_services",
        "java_generic_services",
        "py_generic_services",
        "deprecated",
        "cc_enable_arenas",
        "objc_class_prefix",
        "csharp_namespace",
        "swift_prefix",
        "php_class_prefix",
        "php_namespace",
        "php_metadata_namespace",
        "ruby_package",
    };
    static const std::vector<std::string_view> message{
        "message_set_wire_format",
        "no_standard_descriptor_accessor",
        "deprecated",
        "map_entry",
        "deprecated_legacy_json_field_conflicts",
    };
    static const std::vector<std::string_view> field{
        "ctype",
        "packed",
        "jstype",
        "lazy",
        "unverified_lazy",
        "deprecated",
        "weak",
        "debug_redact",
        "retention",
        "targets",
        "default",
        "json_name",
        "edition_defaults",
        "feature_support",
    };
    static const std::vector<std::string_view> enumeration{
        "allow_alias",
        "deprecated",
        "deprecated_legacy_json_field_conflicts",
    };
    static const std::vector<std::string_view> value{ "deprecated", "debug_redact" };
    static const std::vector<std::string_view> service{ "deprecated" };
    static const std::vector<std::string_view> method{ "deprecated", "idempotency_level" };
    static const std::vector<std::string_view> range{ "declaration", "verification" };
    static const std::vector<std::string_view> none;
    if (where == "file") {
        return file;
    }

    if (where == "message") {
        return message;
    }

    if (where == "field") {
        return field;
    }

    if (where == "enum") {
        return enumeration;
    }

    if (where == "enum value") {
        return value;
    }

    if (where == "service") {
        return service;
    }

    if (where == "extension range") {
        return range;
    }

    return where == "rpc" ? method : none;
}

/// An option as written: its name, and its value.
struct option_t {
    /// Its name, like "packed", "(my.opt)", or "(my.opt).part".
    std::string name;
    /// A simple name: not a custom option's, in parentheses.
    bool simple = false;
    span_t name_span;
    proto_constant_t value;
};

/// Reads a .proto's tokens into a proto_file_t.
class proto_parser_t {
public:
    proto_parser_t(const source_file_t& source, std::vector<proto_token_t> tokens,
                   diagnostic_bag_t& diagnostics)
        : m_source(source), m_tokens(std::move(tokens)), m_diagnostics(diagnostics) {}

    proto_file_t run() {
        m_file.file = m_source.id();
        for (const proto_token_t& token : m_tokens) {
            if (token.kind == lexeme_t::IDENTIFIER) {
                m_file.identifiers.emplace_back(token.text);
            }
        }

        std::ranges::sort(m_file.identifiers);
        const auto [first, last] = std::ranges::unique(m_file.identifiers);
        m_file.identifiers.erase(first, last);
        bool first_statement = true;
        bool syntax = false;
        while (!at_end()) {
            const std::size_t errors = m_diagnostics.error_count();
            if (accept_symbol(';')) {
                continue;
            }

            if (looking_at("syntax")) {
                if (!first_statement) {
                    error(peek().span, "the syntax line comes first in a .proto file");
                }

                parse_syntax();
                syntax = true;
            } else if (looking_at("edition")) {
                error(peek().span, "editions aren't supported; a .proto file says syntax = "
                                   "\"proto2\" or \"proto3\"");
                skip_statement();
            } else {
                if (first_statement && !syntax) {
                    m_diagnostics.warning(
                        peek().span,
                        "a .proto file without a syntax line is proto2; say syntax = \"proto2\" "
                        "or syntax = \"proto3\" first");
                }

                parse_top_level();
            }

            first_statement = false;
            if (m_diagnostics.error_count() > errors) {
                m_failed = true;
            }
        }

        return std::move(m_file);
    }

    /// Whether anything it read had an error.
    [[nodiscard]] bool failed() const {
        return m_failed;
    }

private:
    const source_file_t& m_source;
    std::vector<proto_token_t> m_tokens;
    diagnostic_bag_t& m_diagnostics;
    proto_file_t m_file;
    std::size_t m_at = 0;
    bool m_package = false;
    bool m_failed = false;

    template <typename... Args>
    void error(const span_t span, std::format_string<Args...> fmt, Args&&... args) {
        m_diagnostics.error(span, fmt, std::forward<Args>(args)...);
        m_failed = true;
    }

    [[nodiscard]] const proto_token_t& peek(const std::size_t ahead = 0) const {
        return m_tokens[std::min(m_at + ahead, m_tokens.size() - 1)];
    }

    [[nodiscard]] bool at_end() const {
        return peek().kind == lexeme_t::END;
    }

    const proto_token_t& advance() {
        const proto_token_t& token = peek();
        if (!at_end()) {
            ++m_at;
        }

        return token;
    }

    [[nodiscard]] bool looking_at(const std::string_view word, const std::size_t ahead = 0) const {
        return peek(ahead).kind == lexeme_t::IDENTIFIER && peek(ahead).text == word;
    }

    [[nodiscard]] bool at_symbol(const char symbol, const std::size_t ahead = 0) const {
        return peek(ahead).kind == lexeme_t::SYMBOL && peek(ahead).text.front() == symbol;
    }

    bool accept_symbol(const char symbol) {
        if (!at_symbol(symbol)) {
            return false;
        }

        advance();
        return true;
    }

    /// What a message says the token is: "'x'", "the number 5", or "the end of the file".
    [[nodiscard]] std::string describe(const proto_token_t& token) const {
        switch (token.kind) {
        case lexeme_t::END:
            return "the end of the file";
        case lexeme_t::STRING:
            return std::format("the string {}", token.text);
        case lexeme_t::INTEGER:
        case lexeme_t::FLOAT:
            return std::format("the number {}", token.text);
        case lexeme_t::IDENTIFIER:
        case lexeme_t::SYMBOL:
            break;
        }

        return std::format("'{}'", token.text);
    }

    /// Reports that what's next isn't `what`, and gives false, so the statement is skipped.
    bool expected(const std::string_view what) {
        error(peek().span, "expected {}, found {}", what, describe(peek()));
        return false;
    }

    bool expect_symbol(const char symbol) {
        if (accept_symbol(symbol)) {
            return true;
        }

        return expected(std::format("'{}'", symbol));
    }

    std::optional<proto_token_t> expect_identifier(const std::string_view what) {
        if (peek().kind != lexeme_t::IDENTIFIER) {
            expected(what);
            return std::nullopt;
        }

        return advance();
    }

    /// Skips the rest of a statement that has an error: to its ';', past a block it opens, or to
    /// the '}' that ends the block it's in, which stays.
    void skip_statement() {
        int depth = 0;
        while (!at_end()) {
            if (at_symbol('}')) {
                if (depth == 0) {
                    return;
                }

                --depth;
                advance();
                if (depth == 0) {
                    return;
                }

                continue;
            }

            if (at_symbol('{')) {
                ++depth;
            } else if (at_symbol(';') && depth == 0) {
                advance();
                return;
            }

            advance();
        }
    }

    /// `syntax = "proto2";` or "proto3".
    void parse_syntax() {
        advance();
        if (!expect_symbol('=')) {
            skip_statement();
            return;
        }

        if (peek().kind != lexeme_t::STRING) {
            expected("\"proto2\" or \"proto3\"");
            skip_statement();
            return;
        }

        const proto_token_t& written = advance();
        if (written.value == "proto3") {
            m_file.syntax = proto_syntax_t::PROTO3;
        } else if (written.value == "proto2") {
            m_file.syntax = proto_syntax_t::PROTO2;
        } else {
            error(written.span, "the syntax is \"proto2\" or \"proto3\", not {}", written.text);
        }

        if (!expect_symbol(';')) {
            skip_statement();
        }
    }

    void parse_top_level() {
        if (looking_at("import")) {
            parse_import();
        } else if (looking_at("package")) {
            parse_package();
        } else if (looking_at("option")) {
            parse_option_statement("file");
        } else if (looking_at("message")) {
            parse_message(std::nullopt);
        } else if (looking_at("enum")) {
            parse_enum(std::nullopt);
        } else if (looking_at("service")) {
            parse_service();
        } else if (looking_at("extend")) {
            parse_extend(std::nullopt);
        } else {
            expected("a declaration, like a message, an enum, or a service");
            skip_statement();
        }
    }

    /// `import "other.proto";`, `import public "other.proto";`, or weak.
    void parse_import() {
        advance();
        proto_import_t imported;
        if (looking_at("public")) {
            advance();
            imported.is_public = true;
        } else if (looking_at("weak")) {
            advance();
        }

        if (peek().kind != lexeme_t::STRING) {
            expected("the path of a .proto file, in quotes");
            skip_statement();
            return;
        }

        const proto_token_t& path = advance();
        imported.path = path.value;
        imported.span = path.span;
        if (!expect_symbol(';')) {
            skip_statement();
            return;
        }

        const auto same = std::ranges::find(m_file.imports, imported.path, &proto_import_t::path);
        if (same != m_file.imports.end()) {
            error(imported.span, "\"{}\" is imported already", imported.path);
            return;
        }

        m_file.imports.push_back(std::move(imported));
    }

    /// A dotted name: "a", "a.b.c", or with `leading`, ".a.b".
    std::optional<std::string> parse_full_name(span_t& span, const bool leading) {
        const std::uint32_t begin = peek().span.begin;
        std::string name;
        if (leading && accept_symbol('.')) {
            name = ".";
        }

        while (true) {
            const std::optional<proto_token_t> part = expect_identifier("a name");
            if (!part) {
                return std::nullopt;
            }

            name += part->text;
            span = { .file = m_file.file, .begin = begin, .end = part->span.end };
            if (!at_symbol('.')) {
                return name;
            }

            advance();
            name += '.';
        }
    }

    /// `package game.net;`, once.
    void parse_package() {
        const proto_token_t& keyword = advance();
        span_t span;
        const std::optional<std::string> name = parse_full_name(span, false);
        if (!name || !expect_symbol(';')) {
            skip_statement();
            return;
        }

        if (m_package) {
            error(keyword.span, "a .proto file has one package line");
            return;
        }

        m_package = true;
        m_file.package = *name;
    }

    /// A constant: a name, a number with or without a sign, strings, or a message's fields in
    /// braces.
    std::optional<proto_constant_t> parse_constant() {
        const proto_token_t& first = peek();
        proto_constant_t constant{ .span = first.span };
        if (first.kind == lexeme_t::STRING) {
            constant.kind = proto_constant_kind_t::STRING;
            while (peek().kind == lexeme_t::STRING) {
                const proto_token_t& part = advance();
                constant.text += part.value;
                constant.span.end = part.span.end;
            }

            return constant;
        }

        if (at_symbol('{')) {
            constant.kind = proto_constant_kind_t::AGGREGATE;
            int depth = 0;
            do {
                if (at_symbol('{')) {
                    ++depth;
                } else if (at_symbol('}')) {
                    --depth;
                }

                constant.span.end = advance().span.end;
            } while (depth > 0 && !at_end());
            if (depth > 0) {
                expected("'}'");
                return std::nullopt;
            }

            return constant;
        }

        bool negative = false;
        if (at_symbol('-') || at_symbol('+')) {
            negative = advance().text == "-";
        }

        const proto_token_t& token = peek();
        constant.negative = negative;
        constant.span.end = token.span.end;
        if ((token.kind == lexeme_t::INTEGER || token.kind == lexeme_t::FLOAT) && token.wrong) {
            advance();
            return std::nullopt;
        }

        if (token.kind == lexeme_t::INTEGER) {
            advance();
            constant.kind = proto_constant_kind_t::INTEGER;
            constant.integer = token.integer;
            constant.text = token.text;
            return constant;
        }

        if (token.kind == lexeme_t::FLOAT) {
            advance();
            constant.kind = proto_constant_kind_t::FLOAT;
            constant.number = negative ? -token.number : token.number;
            constant.text = token.text;
            return constant;
        }

        if (token.kind == lexeme_t::IDENTIFIER) {
            span_t span;
            std::optional<std::string> name = parse_full_name(span, false);
            if (!name) {
                return std::nullopt;
            }

            constant.span.end = span.end;
            if (negative && (*name == "inf" || *name == "nan")) {
                constant.kind = proto_constant_kind_t::FLOAT;
                constant.number = *name == "inf" ? -std::numeric_limits<double>::infinity()
                                                 : std::numeric_limits<double>::quiet_NaN();
                constant.text = *name;
                return constant;
            }

            if (negative) {
                error(constant.span, "a '-' goes before a number, not {}", *name);
                return std::nullopt;
            }

            constant.kind = proto_constant_kind_t::IDENTIFIER;
            constant.text = std::move(*name);
            return constant;
        }

        expected("a value: a number, a name, or a string");
        return std::nullopt;
    }

    /// An option's name: `packed`, `(my.option)`, or `(my.option).part`.
    std::optional<option_t> parse_option_name() {
        option_t option{ .simple = true, .name_span = peek().span };
        while (true) {
            if (accept_symbol('(')) {
                option.simple = false;
                span_t span;
                const std::optional<std::string> name = parse_full_name(span, true);
                if (!name || !expect_symbol(')')) {
                    return std::nullopt;
                }

                option.name += std::format("({})", *name);
            } else {
                const std::optional<proto_token_t> part = expect_identifier("an option's name");
                if (!part) {
                    return std::nullopt;
                }

                option.name += part->text;
            }

            option.name_span.end = m_tokens[m_at - 1].span.end;
            if (!at_symbol('.')) {
                return option;
            }

            advance();
            option.simple = false;
            option.name += '.';
        }
    }

    /// `name = value`, in an option statement or brackets.
    std::optional<option_t> parse_option() {
        std::optional<option_t> option = parse_option_name();
        if (!option || !expect_symbol('=')) {
            return std::nullopt;
        }

        std::optional<proto_constant_t> value = parse_constant();
        if (!value) {
            return std::nullopt;
        }

        option->value = std::move(*value);
        return option;
    }

    /// Checks an option's name, which `where` takes, as protobuf's descriptor.proto names them.
    /// False when it isn't one.
    bool check_option(const option_t& option, const std::string_view where) {
        if (option.name.starts_with('(')) {
            return true;
        }

        const std::string_view first =
            std::string_view(option.name)
                .substr(0, std::min(option.name.find('.'), option.name.size()));
        if (first == "features") {
            error(option.name_span, "features are an edition's, and a .proto file here is proto2 "
                                    "or proto3");
            return false;
        }

        if (!std::ranges::contains(known_options(where), first)) {
            const bool vowel = where.front() == 'e' || where == "rpc";
            error(option.name_span, "'{}' isn't an option of {} {}", option.name,
                  vowel ? "an" : "a", where);
            return false;
        }

        return true;
    }

    /// Whether a bool option is true, reporting a value that isn't true or false.
    bool bool_option(const option_t& option) {
        if (option.value.kind == proto_constant_kind_t::IDENTIFIER &&
            (option.value.text == "true" || option.value.text == "false")) {
            return option.value.text == "true";
        }

        error(option.value.span, "{} is true or false", option.name);
        return false;
    }

    /// `option name = value;` in a file, message, enum, service, rpc, or oneof, which `where`
    /// names. Gives the option when it reads.
    std::optional<option_t> parse_option_statement(const std::string_view where) {
        advance();
        std::optional<option_t> option = parse_option();
        if (!option || !expect_symbol(';')) {
            skip_statement();
            return std::nullopt;
        }

        if (!check_option(*option, where)) {
            return std::nullopt;
        }

        return option;
    }

    /// `[name = value, ...]` after a field, an enum's value, or extensions.
    std::vector<option_t> parse_bracket_options(const std::string_view where, bool& ok) {
        std::vector<option_t> options;
        advance();
        while (true) {
            std::optional<option_t> option = parse_option();
            if (!option) {
                ok = false;
                return options;
            }

            if (check_option(*option, where)) {
                options.push_back(std::move(*option));
            }

            if (accept_symbol(']')) {
                return options;
            }

            if (!accept_symbol(',')) {
                expected("',' or ']'");
                ok = false;
                return options;
            }
        }
    }

    /// The full name of what's declared in a message, or at the top level: "pkg.Outer.Name".
    [[nodiscard]] std::string full_name(const std::optional<std::size_t> parent,
                                        const std::string_view name) const {
        if (parent) {
            return std::format("{}.{}", m_file.messages[*parent].full_name, name);
        }

        return m_file.package.empty() ? std::string(name)
                                      : std::format("{}.{}", m_file.package, name);
    }

    /// `message Name { ... }`, and the messages and enums in it, after it.
    void parse_message(const std::optional<std::size_t> parent) {
        advance();
        const std::optional<proto_token_t> name = expect_identifier("the message's name");
        if (!name) {
            skip_statement();
            return;
        }

        const std::size_t index = m_file.messages.size();
        m_file.messages.push_back({
            .name = std::string(name->text),
            .name_span = name->span,
            .full_name = full_name(parent, name->text),
            .parent = parent,
        });
        if (!expect_symbol('{')) {
            skip_statement();
            return;
        }

        parse_message_body(index);
    }

    /// A message's declarations, up to its '}'.
    void parse_message_body(const std::size_t index) {
        while (!accept_symbol('}')) {
            if (at_end()) {
                expected("'}' at the end of the message");
                return;
            }

            if (accept_symbol(';')) {
                continue;
            }

            if (looking_at("message") && peek(1).kind == lexeme_t::IDENTIFIER) {
                parse_message(index);
            } else if (looking_at("enum") && peek(1).kind == lexeme_t::IDENTIFIER) {
                parse_enum(index);
            } else if (looking_at("oneof") && peek(1).kind == lexeme_t::IDENTIFIER) {
                parse_oneof(index);
            } else if (looking_at("reserved")) {
                parse_reserved(m_file.messages[index].reserved,
                               m_file.messages[index].reserved_names, max_field_number);
            } else if (looking_at("extensions")) {
                parse_extensions(index);
            } else if (looking_at("extend")) {
                parse_extend(index);
            } else if (looking_at("option")) {
                if (const std::optional<option_t> option = parse_option_statement("message")) {
                    message_option(index, *option);
                }
            } else {
                parse_field(index, std::nullopt);
            }
        }

        check_numbers(index);
    }

    void message_option(const std::size_t index, const option_t& option) {
        if (option.name == "deprecated") {
            m_file.messages[index].deprecated = bool_option(option);
        } else if (option.name == "map_entry") {
            error(option.name_span, "map_entry isn't set by hand; a map field is map<K, V>");
        } else if (option.name == "message_set_wire_format" && bool_option(option)) {
            error(option.name_span, "a message set's wire format isn't supported");
        }
    }

    /// A field's type: a scalar's name, or a message's or enum's, maybe with dots.
    std::optional<proto_type_t> parse_type() {
        span_t span = peek().span;
        std::optional<std::string> name = parse_full_name(span, true);
        if (!name) {
            return std::nullopt;
        }

        if (const std::optional<proto_kind_t> scalar = scalar_kind(*name)) {
            return proto_type_t{ .kind = *scalar, .span = span };
        }

        return proto_type_t{ .kind = proto_kind_t::NAMED, .name = std::move(*name), .span = span };
    }

    /// A field, a map, or a group, in a message, or in one of its oneofs. Gives false after an
    /// error.
    bool parse_field(const std::size_t index, const std::optional<std::size_t> oneof) {
        if (!parse_field_parts(index, oneof)) {
            skip_statement();
            return false;
        }

        return true;
    }

    bool parse_field_parts(const std::size_t index, const std::optional<std::size_t> oneof) {
        proto_field_t field{ .oneof = oneof };
        const proto_token_t& start = peek();
        if (looking_at("optional") || looking_at("required") || looking_at("repeated")) {
            const proto_token_t& label = advance();
            field.label = proto_label_t::REPEATED;
            if (label.text == "optional") {
                field.label = proto_label_t::OPTIONAL;
            } else if (label.text == "required") {
                field.label = proto_label_t::REQUIRED;
            }

            if (oneof) {
                error(label.span, "a oneof's fields take no label: they're each optional");
            } else if (field.label == proto_label_t::REQUIRED &&
                       m_file.syntax == proto_syntax_t::PROTO3) {
                error(label.span, "proto3 has no required fields");
            }
        }

        if (looking_at("map") && at_symbol('<', 1)) {
            return parse_map(index, std::move(field), start);
        }

        if (looking_at("group") && peek(1).kind == lexeme_t::IDENTIFIER) {
            return parse_group(index, std::move(field), start);
        }

        if (field.label == proto_label_t::NONE && !oneof &&
            m_file.syntax == proto_syntax_t::PROTO2) {
            error(start.span, "a proto2 field starts with its label: required, optional, or "
                              "repeated");
        }

        std::optional<proto_type_t> type = parse_type();
        if (!type) {
            return false;
        }

        field.type = std::move(*type);
        return finish_field(index, std::move(field));
    }

    /// `name = number [options];`, after a field's type.
    bool finish_field(const std::size_t index, proto_field_t field) {
        const std::optional<proto_token_t> name = expect_identifier("the field's name");
        if (!name || !expect_symbol('=')) {
            return false;
        }

        field.name = name->text;
        field.name_span = name->span;
        if (!parse_number(field)) {
            return false;
        }

        if (at_symbol('[') && !parse_field_options(field)) {
            return false;
        }

        if (!expect_symbol(';')) {
            return false;
        }

        m_file.messages[index].fields.push_back(std::move(field));
        return true;
    }

    /// A field's number, from 1 to 536870911, outside 19000 to 19999.
    bool parse_number(proto_field_t& field) {
        if (peek().kind != lexeme_t::INTEGER) {
            return expected("the field's number");
        }

        const proto_token_t& number = advance();
        field.number_span = number.span;
        if (number.wrong) {
            return false;
        }

        if (number.integer == 0 || number.integer > max_field_number) {
            error(number.span, "a field's number is from 1 to {}, not {}", max_field_number,
                  number.text);
            return true;
        }

        field.number = static_cast<std::int32_t>(number.integer);
        if (field.number >= first_reserved_number && field.number <= last_reserved_number) {
            error(number.span, "field numbers {} to {} are protobuf's own", first_reserved_number,
                  last_reserved_number);
        }

        return true;
    }

    bool parse_field_options(proto_field_t& field) {
        bool ok = true;
        for (const option_t& option : parse_bracket_options("field", ok)) {
            if (option.name == "default") {
                if (field.default_value) {
                    error(option.name_span, "the field's default is given already");
                }

                field.default_value = option.value;
                check_default_place(field, option);
            } else if (option.name == "packed") {
                // Whether a type's name is a message's or an enum's waits for resolve_proto().
                field.packed = bool_option(option);
                if (field.label != proto_label_t::REPEATED || field.key ||
                    field.type.kind == proto_kind_t::STRING ||
                    field.type.kind == proto_kind_t::BYTES) {
                    error(option.name_span, "only a repeated field of numbers, bools, or enums is "
                                            "packed");
                }
            } else if (option.name == "deprecated") {
                field.deprecated = bool_option(option);
            } else if (option.name == "json_name" &&
                       option.value.kind != proto_constant_kind_t::STRING) {
                error(option.value.span, "json_name is a string");
            }
        }

        return ok;
    }

    /// Where a default can go: a proto2 field that isn't repeated, a map, or a group.
    void check_default_place(const proto_field_t& field, const option_t& option) {
        if (m_file.syntax == proto_syntax_t::PROTO3) {
            error(option.name_span, "proto3's fields have no defaults: each one's is its zero");
        } else if (field.label == proto_label_t::REPEATED || field.key) {
            error(option.name_span, "a repeated field has no default");
        } else if (field.group) {
            error(option.name_span, "a group has no default");
        }
    }

    /// `map<K, V> name = number;`.
    bool parse_map(const std::size_t index, proto_field_t field, const proto_token_t& start) {
        advance(); // map
        advance(); // <
        if (field.label != proto_label_t::NONE) {
            error(start.span, "a map field takes no label: it's repeated already");
        }

        if (field.oneof) {
            error(start.span, "a oneof can't have a map field");
        }

        std::optional<proto_type_t> key = parse_type();
        if (!key || !expect_symbol(',')) {
            return false;
        }

        std::optional<proto_type_t> value = parse_type();
        if (!value || !expect_symbol('>')) {
            return false;
        }

        if (key->kind == proto_kind_t::NAMED || key->kind == proto_kind_t::FLOAT ||
            key->kind == proto_kind_t::DOUBLE || key->kind == proto_kind_t::BYTES) {
            error(key->span, "a map's key is an integer, a bool, or a string, not {}",
                  key->kind == proto_kind_t::NAMED ? key->name : scalar_name(key->kind));
        }

        field.label = proto_label_t::REPEATED;
        field.key = std::move(*key);
        field.type = std::move(*value);
        return finish_field(index, std::move(field));
    }

    /// `optional group Name = number { ... }`, proto2's: a field whose message its braces
    /// declare, named after it in lower case.
    bool parse_group(const std::size_t index, proto_field_t field, const proto_token_t& start) {
        advance(); // group
        const proto_token_t& name = advance();
        if (m_file.syntax == proto_syntax_t::PROTO3) {
            error(start.span, "proto3 has no groups; use a message field");
        }

        if (field.label == proto_label_t::NONE && !field.oneof) {
            error(start.span, "a group starts with its label: required, optional, or repeated");
        }

        if (name.text.front() < 'A' || name.text.front() > 'Z') {
            error(name.span, "a group's name starts with a capital letter");
        }

        if (!expect_symbol('=')) {
            return false;
        }

        field.group = true;
        field.name = name.text;
        std::ranges::transform(field.name, field.name.begin(), [](const char c) {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        });
        field.name_span = name.span;
        if (!parse_number(field) || (at_symbol('[') && !parse_field_options(field)) ||
            !expect_symbol('{')) {
            return false;
        }

        const std::size_t group = m_file.messages.size();
        m_file.messages.push_back({
            .name = std::string(name.text),
            .name_span = name.span,
            .full_name = full_name(index, name.text),
            .parent = index,
            .group = true,
        });
        field.type = {
            .kind = proto_kind_t::NAMED,
            .name = std::string(name.text),
            .span = name.span,
        };
        m_file.messages[index].fields.push_back(std::move(field));
        parse_message_body(group);
        return true;
    }

    /// `oneof name { ... }`: fields, and options.
    void parse_oneof(const std::size_t index) {
        advance();
        const proto_token_t& name = advance();
        const std::size_t oneof = m_file.messages[index].oneofs.size();
        m_file.messages[index].oneofs.push_back(
            { .name = std::string(name.text), .name_span = name.span });
        if (!expect_symbol('{')) {
            skip_statement();
            return;
        }

        const std::size_t before = m_file.messages[index].fields.size();
        while (!accept_symbol('}')) {
            if (at_end()) {
                expected("'}' at the end of the oneof");
                return;
            }

            if (accept_symbol(';')) {
                continue;
            }

            if (looking_at("option")) {
                parse_option_statement("oneof");
            } else {
                parse_field(index, oneof);
            }
        }

        const bool empty =
            std::ranges::none_of(m_file.messages[index].fields | std::views::drop(before),
                                 [&](const proto_field_t& field) { return field.oneof == oneof; });
        if (empty) {
            error(name.span, "a oneof needs a field");
        }
    }

    /// `reserved 2, 15, 9 to 11, 40 to max;` or `reserved "foo", "bar";`.
    void parse_reserved(std::vector<proto_range_t>& ranges, std::vector<std::string>& names,
                        const std::int64_t max) {
        advance();
        if (peek().kind == lexeme_t::STRING) {
            while (true) {
                names.push_back(advance().value);
                if (accept_symbol(';')) {
                    return;
                }

                if (!expect_symbol(',')) {
                    skip_statement();
                    return;
                }

                if (peek().kind != lexeme_t::STRING) {
                    expected("a reserved name, in quotes");
                    skip_statement();
                    return;
                }
            }
        }

        if (peek().kind == lexeme_t::IDENTIFIER) {
            error(peek().span, "a reserved name goes in quotes in proto2 and proto3: \"{}\"",
                  peek().text);
            skip_statement();
            return;
        }

        if (!parse_ranges(ranges, max) || !expect_symbol(';')) {
            skip_statement();
        }
    }

    /// Ranges of numbers, `5`, `9 to 11`, or `40 to max`, with commas between.
    bool parse_ranges(std::vector<proto_range_t>& ranges, const std::int64_t max) {
        while (true) {
            proto_range_t range{ .span = peek().span };
            std::optional<std::int64_t> low = parse_range_number(max);
            if (!low) {
                return false;
            }

            std::int64_t high = *low;
            if (looking_at("to")) {
                advance();
                if (looking_at("max")) {
                    range.span.end = advance().span.end;
                    high = max;
                } else {
                    const std::optional<std::int64_t> end = parse_range_number(max);
                    if (!end) {
                        return false;
                    }

                    high = *end;
                    range.span.end = m_tokens[m_at - 1].span.end;
                }
            }

            if (high < *low) {
                error(range.span, "a range ends at a number no lower than where it starts");
            }

            range.low = static_cast<std::int32_t>(*low);
            range.high = static_cast<std::int32_t>(high);
            ranges.push_back(range);
            if (!accept_symbol(',')) {
                return true;
            }
        }
    }

    /// A number in a range, from 1 for a message's, up to `max`.
    std::optional<std::int64_t> parse_range_number(const std::int64_t max) {
        const span_t span = peek().span;
        bool negative = false;
        if (max != max_field_number && accept_symbol('-')) {
            negative = true;
        }

        if (peek().kind != lexeme_t::INTEGER) {
            expected("a number");
            return std::nullopt;
        }

        const proto_token_t& number = advance();
        if (number.wrong) {
            return std::nullopt;
        }

        const std::int64_t low =
            max == max_field_number ? 1 : std::numeric_limits<std::int32_t>::min();
        if (number.integer > static_cast<std::uint64_t>(max) + (negative ? 1 : 0)) {
            error(merge(span, number.span), "{}{} is past {}", negative ? "-" : "", number.text,
                  max);
            return std::nullopt;
        }

        const std::int64_t value = negative ? -static_cast<std::int64_t>(number.integer)
                                            : static_cast<std::int64_t>(number.integer);
        if (value < low) {
            error(merge(span, number.span), "a field's number is from 1 to {}", max);
            return std::nullopt;
        }

        return value;
    }

    /// `extensions 100 to 199;`, whose numbers the message's own fields don't take.
    void parse_extensions(const std::size_t index) {
        advance();
        std::vector<proto_range_t> ranges;
        if (!parse_ranges(ranges, max_field_number)) {
            skip_statement();
            return;
        }

        if (at_symbol('[')) {
            bool ok = true;
            parse_bracket_options("extension range", ok);
            if (!ok) {
                skip_statement();
                return;
            }
        }

        if (!expect_symbol(';')) {
            skip_statement();
            return;
        }

        std::ranges::copy(ranges, std::back_inserter(m_file.messages[index].extensions));
    }

    /// `extend Type { fields }`: fields another message gets, which fb reads past. The message
    /// keeps them as fields it doesn't know.
    void parse_extend(const std::optional<std::size_t> parent) {
        advance();
        if (!parse_type() || !expect_symbol('{')) {
            skip_statement();
            return;
        }

        // The fields go in a message of their own, which is dropped.
        const std::size_t scratch = m_file.messages.size();
        m_file.messages.push_back({ .name = "", .parent = parent });
        while (!accept_symbol('}')) {
            if (at_end()) {
                expected("'}' at the end of the extend");
                break;
            }

            if (!accept_symbol(';')) {
                parse_field(scratch, std::nullopt);
            }
        }

        // A group's message comes after it.
        m_file.messages.erase(m_file.messages.begin() + static_cast<std::ptrdiff_t>(scratch),
                              m_file.messages.end());
    }

    /// `enum Name { VALUE = 0; ... }`.
    void parse_enum(const std::optional<std::size_t> parent) {
        advance();
        const std::optional<proto_token_t> name = expect_identifier("the enum's name");
        if (!name || !expect_symbol('{')) {
            skip_statement();
            return;
        }

        proto_enum_t declared{
            .name = std::string(name->text),
            .name_span = name->span,
            .full_name = full_name(parent, name->text),
            .parent = parent,
        };
        while (!accept_symbol('}')) {
            if (at_end()) {
                expected("'}' at the end of the enum");
                break;
            }

            if (accept_symbol(';')) {
                continue;
            }

            if (looking_at("option")) {
                if (const std::optional<option_t> option = parse_option_statement("enum")) {
                    if (option->name == "allow_alias") {
                        declared.allow_alias = bool_option(*option);
                    } else if (option->name == "deprecated") {
                        declared.deprecated = bool_option(*option);
                    }
                }
            } else if (looking_at("reserved")) {
                parse_reserved(declared.reserved, declared.reserved_names,
                               std::numeric_limits<std::int32_t>::max());
            } else if (!parse_enum_value(declared)) {
                skip_statement();
            }
        }

        check_enum(declared);
        m_file.enums.push_back(std::move(declared));
    }

    /// `NAME = number [options];`.
    bool parse_enum_value(proto_enum_t& declared) {
        const std::optional<proto_token_t> name = expect_identifier("a value's name");
        if (!name || !expect_symbol('=')) {
            return false;
        }

        const span_t start = peek().span;
        const bool negative = accept_symbol('-');
        if (peek().kind != lexeme_t::INTEGER) {
            return expected("the value's number");
        }

        const proto_token_t& number = advance();
        if (number.wrong) {
            return false;
        }

        const std::uint64_t limit = negative ? 2147483648ULL : 2147483647ULL;
        if (number.integer > limit) {
            error(merge(start, number.span), "an enum's value is an int32, which {}{} isn't",
                  negative ? "-" : "", number.text);
            return false;
        }

        if (at_symbol('[')) {
            bool ok = true;
            parse_bracket_options("enum value", ok);
            if (!ok) {
                return false;
            }
        }

        if (!expect_symbol(';')) {
            return false;
        }

        const auto value = static_cast<std::int64_t>(number.integer);
        declared.values.push_back({
            .name = std::string(name->text),
            .name_span = name->span,
            .number = static_cast<std::int32_t>(negative ? -value : value),
        });
        return true;
    }

    /// An enum has a value, proto3's first is 0, numbers repeat only with allow_alias, and
    /// reserved numbers and names aren't used.
    void check_enum(const proto_enum_t& declared) {
        if (declared.values.empty()) {
            error(declared.name_span, "an enum needs a value");
            return;
        }

        if (m_file.syntax == proto_syntax_t::PROTO3 && declared.values.front().number != 0) {
            error(declared.values.front().name_span,
                  "a proto3 enum's first value is 0, its zero, which a field it doesn't set has");
        }

        bool aliased = false;
        for (std::size_t i = 0; i < declared.values.size(); ++i) {
            const proto_enum_value_t& value = declared.values[i];
            for (std::size_t j = 0; j < i; ++j) {
                if (declared.values[j].number != value.number) {
                    continue;
                }

                aliased = true;
                if (!declared.allow_alias) {
                    error(value.name_span,
                          "{} has {}'s number, {}; values share one only with option "
                          "allow_alias = true",
                          value.name, declared.values[j].name, value.number);
                }

                break;
            }

            for (const proto_range_t& range : declared.reserved) {
                if (value.number >= range.low && value.number <= range.high) {
                    error(value.name_span, "{}'s number, {}, is reserved", value.name,
                          value.number);
                }
            }

            if (std::ranges::contains(declared.reserved_names, value.name)) {
                error(value.name_span, "the name {} is reserved", value.name);
            }
        }

        if (declared.allow_alias && !aliased) {
            error(declared.name_span,
                  "{} sets allow_alias, but no two of its values share a number", declared.name);
        }
    }

    /// A message's fields have numbers of their own, outside its reserved and extension ranges,
    /// and names it doesn't reserve.
    void check_numbers(const std::size_t index) {
        const proto_message_t& message = m_file.messages[index];
        for (std::size_t i = 0; i < message.fields.size(); ++i) {
            const proto_field_t& field = message.fields[i];
            if (field.number == 0) {
                continue;
            }

            for (std::size_t j = 0; j < i; ++j) {
                if (message.fields[j].number == field.number) {
                    error(field.number_span, "{} has {}'s number, {}", field.name,
                          message.fields[j].name, field.number);
                    break;
                }
            }

            for (const proto_range_t& range : message.reserved) {
                if (field.number >= range.low && field.number <= range.high) {
                    error(field.number_span, "{}'s number, {}, is reserved", field.name,
                          field.number);
                }
            }

            for (const proto_range_t& range : message.extensions) {
                if (field.number >= range.low && field.number <= range.high) {
                    error(field.number_span, "{}'s number, {}, is in an extension range",
                          field.name, field.number);
                }
            }

            if (std::ranges::contains(message.reserved_names, field.name)) {
                error(field.name_span, "the name {} is reserved", field.name);
            }
        }
    }

    /// `service Name { rpc Method(Request) returns (Response); ... }`.
    void parse_service() {
        advance();
        const std::optional<proto_token_t> name = expect_identifier("the service's name");
        if (!name || !expect_symbol('{')) {
            skip_statement();
            return;
        }

        proto_service_t service{
            .name = std::string(name->text),
            .name_span = name->span,
            .full_name = full_name(std::nullopt, name->text),
        };
        while (!accept_symbol('}')) {
            if (at_end()) {
                expected("'}' at the end of the service");
                break;
            }

            if (accept_symbol(';')) {
                continue;
            }

            if (looking_at("option")) {
                if (const std::optional<option_t> option = parse_option_statement("service")) {
                    if (option->name == "deprecated") {
                        service.deprecated = bool_option(*option);
                    }
                }
            } else if (looking_at("rpc")) {
                if (!parse_rpc(service)) {
                    skip_statement();
                }
            } else {
                expected("an rpc");
                skip_statement();
            }
        }

        m_file.services.push_back(std::move(service));
    }

    /// `rpc Name(Request) returns (Response);`, either with `stream`, and options in braces.
    bool parse_rpc(proto_service_t& service) {
        advance();
        const std::optional<proto_token_t> name = expect_identifier("the rpc's name");
        if (!name) {
            return false;
        }

        proto_method_t method{ .name = std::string(name->text), .name_span = name->span };
        if (!expect_symbol('(')) {
            return false;
        }

        if (looking_at("stream") && (peek(1).kind == lexeme_t::IDENTIFIER || at_symbol('.', 1))) {
            advance();
            method.client_streaming = true;
        }

        std::optional<proto_type_t> input = parse_type();
        if (!input || !expect_symbol(')')) {
            return false;
        }

        if (!looking_at("returns")) {
            return expected("'returns'");
        }

        advance();
        if (!expect_symbol('(')) {
            return false;
        }

        if (looking_at("stream") && (peek(1).kind == lexeme_t::IDENTIFIER || at_symbol('.', 1))) {
            advance();
            method.server_streaming = true;
        }

        std::optional<proto_type_t> output = parse_type();
        if (!output || !expect_symbol(')')) {
            return false;
        }

        method.input = std::move(*input);
        method.output = std::move(*output);
        if (accept_symbol('{')) {
            while (!accept_symbol('}')) {
                if (at_end()) {
                    return expected("'}' at the end of the rpc");
                }

                if (accept_symbol(';')) {
                    continue;
                }

                if (!looking_at("option")) {
                    return expected("an option");
                }

                if (const std::optional<option_t> option = parse_option_statement("rpc")) {
                    if (option->name == "deprecated") {
                        method.deprecated = bool_option(*option);
                    }
                }
            }
        } else if (!expect_symbol(';')) {
            return false;
        }

        for (const proto_method_t& other : service.methods) {
            if (other.name == method.name) {
                error(method.name_span, "{} has an rpc named {} already", service.name,
                      method.name);
                return true;
            }
        }

        service.methods.push_back(std::move(method));
        return true;
    }
};

} // namespace

proto_file_t parse_proto(const source_file_t& file, diagnostic_bag_t& diagnostics) {
    std::vector<proto_token_t> tokens = proto_lexer_t(file, diagnostics).run();
    return proto_parser_t(file, std::move(tokens), diagnostics).run();
}

namespace {

/// What a name in a .proto's scope is.
enum class symbol_kind_t : std::uint8_t {
    PACKAGE,
    MESSAGE,
    ENUM,
    ENUM_VALUE,
    FIELD,
    ONEOF,
    SERVICE,
    METHOD,
};

struct proto_symbol_t {
    symbol_kind_t kind = symbol_kind_t::PACKAGE;
    const proto_file_t* file = nullptr;
    /// A message's or enum's index in its file's.
    std::size_t index = 0;
};

/// What messages call a kind of symbol.
std::string_view symbol_kind_name(const symbol_kind_t kind) {
    switch (kind) {
    case symbol_kind_t::PACKAGE:
        return "a package";
    case symbol_kind_t::MESSAGE:
        return "a message";
    case symbol_kind_t::ENUM:
        return "an enum";
    case symbol_kind_t::ENUM_VALUE:
        return "an enum's value";
    case symbol_kind_t::FIELD:
        return "a field";
    case symbol_kind_t::ONEOF:
        return "a oneof";
    case symbol_kind_t::SERVICE:
        return "a service";
    case symbol_kind_t::METHOD:
        return "an rpc";
    }

    return "";
}

/// Where in a full name its last part starts: "pkg.Outer" for "pkg.Outer.Inner", empty for a
/// name without dots.
std::string_view scope_of(const std::string_view name) {
    const std::size_t dot = name.rfind('.');
    return dot == std::string_view::npos ? std::string_view() : name.substr(0, dot);
}

std::string joined(const std::string_view scope, const std::string_view name) {
    return scope.empty() ? std::string(name) : std::format("{}.{}", scope, name);
}

/// The names a .proto file can see, by their full names: its own declarations, and those of the
/// files it imports, and of the files they import publicly.
class proto_scope_t {
public:
    proto_scope_t(const proto_file_t& file, diagnostic_bag_t& diagnostics)
        : m_file(file), m_diagnostics(diagnostics) {
        std::vector<const proto_file_t*> seen{ &file };
        for (const proto_import_t& imported : file.imports) {
            add_visible(imported.file, seen);
        }

        for (const proto_file_t* other : seen | std::views::drop(1)) {
            add_file(*other, false);
        }

        add_file(file, true);
    }

    /// Finds a type by protobuf's rules for `name` written in `scope`: a name with a leading
    /// '.' is a full name; otherwise its first part is looked for in `scope`, then in each scope
    /// around it, and the rest of it in what that finds. Reports a name that isn't a message or
    /// an enum.
    std::optional<proto_symbol_t> find_type(const std::string_view name, const std::string& scope,
                                            const span_t span) {
        if (name.starts_with('.')) {
            return found_type(name.substr(1), name, span);
        }

        const std::string_view first = name.substr(0, std::min(name.find('.'), name.size()));
        std::string_view tried = scope;
        while (true) {
            const std::string candidate = joined(tried, first);
            if (const auto symbol = m_symbols.find(candidate); symbol != m_symbols.end()) {
                const symbol_kind_t kind = symbol->second.kind;
                if (first.size() < name.size()) {
                    if (kind == symbol_kind_t::PACKAGE || kind == symbol_kind_t::MESSAGE) {
                        const std::string full = joined(tried, name);
                        if (!m_symbols.contains(full)) {
                            m_diagnostics.error(span,
                                                "'{}' is {} here, which isn't declared; the scope "
                                                "nearest it is looked in first, and a '.' before "
                                                "a name, as in '.{}', starts from the outermost",
                                                name, full, name);
                            return std::nullopt;
                        }

                        return found_type(full, name, span);
                    }
                } else if (kind == symbol_kind_t::MESSAGE || kind == symbol_kind_t::ENUM) {
                    return symbol->second;
                }
            }

            if (tried.empty()) {
                break;
            }

            tried = scope_of(tried);
        }

        m_diagnostics.error(span,
                            "'{}' isn't a message or an enum that this file declares or "
                            "imports",
                            name);
        return std::nullopt;
    }

private:
    const proto_file_t& m_file;
    diagnostic_bag_t& m_diagnostics;
    std::map<std::string, proto_symbol_t, std::less<>> m_symbols;

    /// The symbol at a full name, which has to be a type.
    std::optional<proto_symbol_t> found_type(const std::string_view full,
                                             const std::string_view written, const span_t span) {
        const auto symbol = m_symbols.find(full);
        if (symbol == m_symbols.end()) {
            m_diagnostics.error(span,
                                "'{}' isn't a message or an enum that this file declares "
                                "or imports",
                                written);
            return std::nullopt;
        }

        if (symbol->second.kind != symbol_kind_t::MESSAGE &&
            symbol->second.kind != symbol_kind_t::ENUM) {
            m_diagnostics.error(span, "'{}' is {}, not a message or an enum", written,
                                symbol_kind_name(symbol->second.kind));
            return std::nullopt;
        }

        return symbol->second;
    }

    /// A file an import names, and those it imports publicly, once each.
    static void add_visible(const proto_file_t* file, std::vector<const proto_file_t*>& seen) {
        if (file == nullptr || std::ranges::contains(seen, file)) {
            return;
        }

        seen.push_back(file);
        for (const proto_import_t& imported : file->imports) {
            if (imported.is_public) {
                add_visible(imported.file, seen);
            }
        }
    }

    /// A file's declarations. Only its own file's clashes are reported, where they're declared.
    void add_file(const proto_file_t& file, const bool own) {
        std::string package;
        for (const auto part : std::views::split(std::string_view(file.package), '.')) {
            if (file.package.empty()) {
                break;
            }

            package = joined(package, std::string_view(part.begin(), part.end()));
            add(package, { .kind = symbol_kind_t::PACKAGE, .file = &file }, {}, false);
        }

        for (std::size_t i = 0; i < file.messages.size(); ++i) {
            const proto_message_t& message = file.messages[i];
            add(message.full_name, { .kind = symbol_kind_t::MESSAGE, .file = &file, .index = i },
                message.name_span, own);
            for (const proto_field_t& field : message.fields) {
                add(joined(message.full_name, field.name),
                    { .kind = symbol_kind_t::FIELD, .file = &file }, field.name_span, own);
            }

            for (const proto_oneof_t& oneof : message.oneofs) {
                add(joined(message.full_name, oneof.name),
                    { .kind = symbol_kind_t::ONEOF, .file = &file }, oneof.name_span, own);
            }
        }

        for (std::size_t i = 0; i < file.enums.size(); ++i) {
            const proto_enum_t& declared = file.enums[i];
            add(declared.full_name, { .kind = symbol_kind_t::ENUM, .file = &file, .index = i },
                declared.name_span, own);
            // An enum's values are in the scope around it, as C++'s are.
            for (const proto_enum_value_t& value : declared.values) {
                add(joined(scope_of(declared.full_name), value.name),
                    { .kind = symbol_kind_t::ENUM_VALUE, .file = &file, .index = i },
                    value.name_span, own);
            }
        }

        for (const proto_service_t& service : file.services) {
            add(service.full_name, { .kind = symbol_kind_t::SERVICE, .file = &file },
                service.name_span, own);
            for (const proto_method_t& method : service.methods) {
                add(joined(service.full_name, method.name),
                    { .kind = symbol_kind_t::METHOD, .file = &file }, method.name_span, own);
            }
        }
    }

    void add(const std::string& name, const proto_symbol_t symbol, const span_t span,
             const bool own) {
        const auto [found, added] = m_symbols.emplace(name, symbol);
        if (added || (symbol.kind == symbol_kind_t::PACKAGE &&
                      found->second.kind == symbol_kind_t::PACKAGE)) {
            return;
        }

        if (!own) {
            return;
        }

        const std::string_view scope = scope_of(name);
        const std::string_view in = scope.empty() ? std::string_view("the file") : scope;
        const std::string where = found->second.file != &m_file
                                      ? std::format(", in \"{}\"", found->second.file->path)
                                      : std::string();
        diagnostic_t& reported =
            m_diagnostics.error(span, "'{}' is declared already in {}{}, as {}",
                                name.substr(scope.size() + (scope.empty() ? 0 : 1)), in, where,
                                symbol_kind_name(found->second.kind));
        if (symbol.kind == symbol_kind_t::ENUM_VALUE ||
            found->second.kind == symbol_kind_t::ENUM_VALUE) {
            reported.with_note("an enum's values are in the scope around the enum, as C++'s are, "
                               "so two enums there can't both have a value of one name");
        }
    }
};

/// The names FutureBlitz reserves that a .proto's names can be: keywords, contextual ones, and
/// the built-in types'.
bool is_reserved(const std::string_view name) {
    static constexpr std::array<std::string_view, 8> others{
        "Void", "Get", "Set", "Step", "Lib", "From", "Weak", "Attribute",
    };
    return keyword_kind(name).has_value() || builtin_type(name).has_value() ||
           std::ranges::contains(others, name);
}

/// A name as FutureBlitz takes it: with a '_' after one it reserves, or that's in `taken`.
std::string fb_name(std::string name, const std::set<std::string, std::less<>>& taken = {}) {
    while (is_reserved(name) || taken.contains(name)) {
        name += '_';
    }

    return name;
}

/// What a message's Struct declares, besides the fields' names, which its own fields can't
/// take: what reads, writes, and shows it.
const std::set<std::string, std::less<>>& struct_members() {
    static const std::set<std::string, std::less<>> members = [] {
        std::set<std::string, std::less<>> names{ "Clone", "Decode", "Encode", "FindMissing" };
        names.insert({ "IsInitialized", "Merge", "MergeFrom", "ReadFrom", "ToString" });
        names.insert({ "WriteText", "WriteTo" });
        return names;
    }();
    return members;
}

/// The FutureBlitz names of a message's members.
struct member_names_t {
    /// Each field's, by index: a Field, or a Property for a oneof's or a boxed one.
    std::vector<std::string> fields;
    /// Each oneof's Property, which says which of its fields holds a value.
    std::vector<std::string> oneofs;
    /// The Property that gives a field's value or its default, for each field with a default;
    /// empty for the others.
    std::vector<std::string> defaults;
};

/// The FutureBlitz names of a message's members: the fields' own, with a '_' after one that's
/// reserved, a member the Struct has, or one of `types`, the module's type names, which its
/// code names.
member_names_t member_names(const proto_message_t& message,
                            const std::set<std::string, std::less<>>& types) {
    std::set<std::string, std::less<>> taken = struct_members();
    taken.insert(types.begin(), types.end());
    member_names_t names;
    for (const proto_field_t& field : message.fields) {
        names.fields.push_back(fb_name(field.name, taken));
    }

    for (const proto_oneof_t& oneof : message.oneofs) {
        names.oneofs.push_back(fb_name(oneof.name, taken));
    }

    for (std::size_t i = 0; i < message.fields.size(); ++i) {
        names.defaults.push_back(message.fields[i].default_value
                                     ? fb_name(names.fields[i] + "OrDefault", taken)
                                     : std::string());
    }

    return names;
}

/// The name of a message's Enum of which of a oneof's fields holds a value.
std::string oneof_enum_name(const proto_file_t& file, const std::size_t message,
                            const std::size_t oneof) {
    return fb_name(std::format("{}_{}", proto_message_name(file, message),
                               file.messages[message].oneofs[oneof].name));
}

/// The FutureBlitz name of a service's Interface.
std::string service_name(const proto_service_t& service) {
    return fb_name(service.name);
}

/// The name of a member of a oneof's Enum that says none of its fields holds a value: NONE,
/// unless a field takes that name.
std::string oneof_none(const member_names_t& names, const proto_message_t& message,
                       const std::size_t oneof) {
    std::set<std::string, std::less<>> members;
    for (std::size_t i = 0; i < message.fields.size(); ++i) {
        if (message.fields[i].oneof == oneof) {
            members.insert(names.fields[i]);
        }
    }

    return fb_name("NONE", members);
}

/// The type names the module of a file declares.
std::set<std::string, std::less<>> module_types(const proto_file_t& file) {
    std::set<std::string, std::less<>> types;
    for (std::size_t i = 0; i < file.messages.size(); ++i) {
        types.insert(proto_message_name(file, i));
        for (std::size_t j = 0; j < file.messages[i].oneofs.size(); ++j) {
            types.insert(oneof_enum_name(file, i, j));
        }
    }

    for (std::size_t i = 0; i < file.enums.size(); ++i) {
        types.insert(proto_enum_name(file, i));
    }

    for (const proto_service_t& service : file.services) {
        types.insert(service_name(service));
    }

    return types;
}

/// The FutureBlitz name of an enum's value: its own, with a '_' after a reserved one.
std::string value_name(const proto_enum_value_t& value) {
    return fb_name(value.name);
}

/// Checks the types a file's fields and rpcs name once they're found, and the names
/// FutureBlitz gives what it declares.
class proto_checker_t {
public:
    proto_checker_t(proto_file_t& file, diagnostic_bag_t& diagnostics)
        : m_file(file), m_diagnostics(diagnostics), m_scope(file, diagnostics) {}

    void run() {
        for (proto_message_t& message : m_file.messages) {
            for (proto_field_t& field : message.fields) {
                resolve(field.type, message.full_name);
                check_field(message, field);
            }
        }

        for (proto_service_t& service : m_file.services) {
            for (proto_method_t& method : service.methods) {
                for (proto_type_t* type : { &method.input, &method.output }) {
                    resolve(*type, service.full_name);
                    if (type->kind == proto_kind_t::ENUM) {
                        m_diagnostics.error(type->span,
                                            "an rpc takes and gives messages, not "
                                            "the enum {}",
                                            type->name);
                    } else if (type->kind != proto_kind_t::MESSAGE &&
                               type->kind != proto_kind_t::NAMED) {
                        m_diagnostics.error(type->span, "an rpc takes and gives messages, not {}",
                                            scalar_name(type->kind));
                    }
                }
            }
        }

        check_names();
    }

private:
    proto_file_t& m_file;
    diagnostic_bag_t& m_diagnostics;
    proto_scope_t m_scope;

    void resolve(proto_type_t& type, const std::string& scope) {
        if (type.kind != proto_kind_t::NAMED) {
            return;
        }

        const std::optional<proto_symbol_t> found = m_scope.find_type(type.name, scope, type.span);
        if (!found) {
            return;
        }

        type.kind =
            found->kind == symbol_kind_t::MESSAGE ? proto_kind_t::MESSAGE : proto_kind_t::ENUM;
        type.file = found->file;
        type.index = found->index;
    }

    /// What depends on a field's type: proto3's enums, packed, and defaults.
    void check_field(const proto_message_t& message, const proto_field_t& field) {
        if (field.type.kind == proto_kind_t::ENUM && m_file.syntax == proto_syntax_t::PROTO3 &&
            field.type.file->syntax == proto_syntax_t::PROTO2) {
            m_diagnostics.error(field.type.span,
                                "a proto3 message's enums are proto3's, which keep numbers "
                                "they don't name; {} is proto2's",
                                field.type.name);
        }

        // A map's entry without a value has the enum's zero, which is its first value.
        if (field.key && field.type.kind == proto_kind_t::ENUM &&
            field.type.file->enums[field.type.index].values.front().number != 0) {
            m_diagnostics.error(field.type.span,
                                "a map's enum values start at 0, which an entry without one has; "
                                "{}'s first value isn't 0",
                                field.type.name);
        }

        if (field.packed && field.type.kind == proto_kind_t::MESSAGE) {
            m_diagnostics.error(field.type.span, "only a repeated field of numbers, bools, or "
                                                 "enums is packed, not messages");
        }

        if (field.default_value && field.label != proto_label_t::REPEATED && !field.key &&
            !field.group && m_file.syntax == proto_syntax_t::PROTO2) {
            check_default(message, field, *field.default_value);
        }
    }

    /// A default fits its field's type.
    void check_default(const proto_message_t& message, const proto_field_t& field,
                       const proto_constant_t& value) {
        const proto_kind_t kind = field.type.kind;
        const auto wrong = [&](const std::string_view what) {
            m_diagnostics.error(value.span, "{}'s default is {}", field.name, what);
        };
        switch (kind) {
        case proto_kind_t::BOOL:
            if (value.kind != proto_constant_kind_t::IDENTIFIER ||
                (value.text != "true" && value.text != "false")) {
                wrong("true or false");
            }

            return;
        case proto_kind_t::STRING:
            if (value.kind != proto_constant_kind_t::STRING) {
                wrong("a string, in quotes");
            } else if (!is_utf8(value.text)) {
                wrong("UTF-8, as every String is");
            }

            return;
        case proto_kind_t::BYTES:
            if (value.kind != proto_constant_kind_t::STRING) {
                wrong("bytes in a string, in quotes");
            }

            return;
        case proto_kind_t::FLOAT:
        case proto_kind_t::DOUBLE:
            if (value.kind != proto_constant_kind_t::INTEGER &&
                value.kind != proto_constant_kind_t::FLOAT &&
                (value.kind != proto_constant_kind_t::IDENTIFIER ||
                 (value.text != "inf" && value.text != "nan"))) {
                wrong("a number");
            }

            return;
        case proto_kind_t::ENUM: {
            const proto_enum_t& declared = field.type.file->enums[field.type.index];
            if (value.kind != proto_constant_kind_t::IDENTIFIER ||
                !std::ranges::contains(declared.values, value.text, &proto_enum_value_t::name)) {
                wrong(std::format("one of {}'s values", declared.name));
            }

            return;
        }
        case proto_kind_t::MESSAGE:
            m_diagnostics.error(value.span, "{} is a message, which has no default", field.name);
            return;
        case proto_kind_t::NAMED:
            return;
        default:
            break;
        }

        check_integer_default(field, value);
        (void)message;
    }

    void check_integer_default(const proto_field_t& field, const proto_constant_t& value) {
        if (value.kind != proto_constant_kind_t::INTEGER) {
            m_diagnostics.error(value.span, "{}'s default is an integer", field.name);
            return;
        }

        const bool is_signed =
            field.type.kind == proto_kind_t::INT32 || field.type.kind == proto_kind_t::SINT32 ||
            field.type.kind == proto_kind_t::SFIXED32 || field.type.kind == proto_kind_t::INT64 ||
            field.type.kind == proto_kind_t::SINT64 || field.type.kind == proto_kind_t::SFIXED64;
        const bool wide =
            field.type.kind == proto_kind_t::INT64 || field.type.kind == proto_kind_t::SINT64 ||
            field.type.kind == proto_kind_t::SFIXED64 || field.type.kind == proto_kind_t::UINT64 ||
            field.type.kind == proto_kind_t::FIXED64;
        std::uint64_t limit = wide ? std::numeric_limits<std::uint64_t>::max()
                                   : std::numeric_limits<std::uint32_t>::max();
        if (is_signed) {
            limit = wide ? (value.negative ? 1ULL << 63 : (1ULL << 63) - 1)
                         : (value.negative ? 1ULL << 31 : (1ULL << 31) - 1);
        } else if (value.negative && value.integer != 0) {
            m_diagnostics.error(value.span, "{} is {}, which has no negative numbers", field.name,
                                scalar_name(field.type.kind));
            return;
        }

        if (value.integer > limit) {
            m_diagnostics.error(value.span, "{}{} doesn't fit in {}, {}'s type",
                                value.negative ? "-" : "", value.text, scalar_name(field.type.kind),
                                field.name);
        }
    }

    /// No two of the module's types, a Struct's members, an Enum's members, or an Interface's
    /// Methods have one FutureBlitz name.
    void check_names() {
        std::map<std::string, span_t, std::less<>> types;
        const auto add_type = [&](const std::string& name, const span_t span,
                                  const std::string_view what) {
            if (!types.emplace(name, span).second) {
                m_diagnostics.error(span,
                                    "{} is {} in FutureBlitz, which another of this file's "
                                    "declarations is too; rename one",
                                    what, name);
            }
        };
        for (std::size_t i = 0; i < m_file.messages.size(); ++i) {
            const proto_message_t& message = m_file.messages[i];
            add_type(proto_message_name(m_file, i), message.name_span, message.full_name);
            for (std::size_t j = 0; j < message.oneofs.size(); ++j) {
                add_type(
                    oneof_enum_name(m_file, i, j), message.oneofs[j].name_span,
                    std::format("{}'s oneof {}'s Enum", message.full_name, message.oneofs[j].name));
            }
        }

        for (std::size_t i = 0; i < m_file.enums.size(); ++i) {
            add_type(proto_enum_name(m_file, i), m_file.enums[i].name_span,
                     m_file.enums[i].full_name);
            check_values(m_file.enums[i]);
        }

        for (const proto_service_t& service : m_file.services) {
            add_type(service_name(service), service.name_span, service.full_name);
            std::set<std::string, std::less<>> methods;
            for (const proto_method_t& method : service.methods) {
                if (!methods.insert(fb_name(method.name)).second) {
                    m_diagnostics.error(method.name_span,
                                        "{} is {} in FutureBlitz, as another of {}'s rpcs is",
                                        method.name, fb_name(method.name), service.name);
                }
            }
        }

        const std::set<std::string, std::less<>> names = module_types(m_file);
        for (const proto_message_t& message : m_file.messages) {
            check_members(message, member_names(message, names));
        }
    }

    void check_values(const proto_enum_t& declared) {
        std::set<std::string, std::less<>> values;
        for (const proto_enum_value_t& value : declared.values) {
            if (!values.insert(value_name(value)).second) {
                m_diagnostics.error(value.name_span,
                                    "{} is {} in FutureBlitz, as another of {}'s values is",
                                    value.name, value_name(value), declared.name);
            }
        }
    }

    void check_members(const proto_message_t& message, const member_names_t& names) {
        std::set<std::string, std::less<>> members;
        const auto add = [&](const std::string& name, const span_t span,
                             const std::string_view what) {
            if (!name.empty() && !members.insert(name).second) {
                m_diagnostics.error(span,
                                    "{} is {} in FutureBlitz, which another of {}'s members "
                                    "is too; rename one",
                                    what, name, message.name);
            }
        };
        for (std::size_t i = 0; i < message.fields.size(); ++i) {
            add(names.fields[i], message.fields[i].name_span, message.fields[i].name);
        }

        for (std::size_t i = 0; i < message.oneofs.size(); ++i) {
            add(names.oneofs[i], message.oneofs[i].name_span, message.oneofs[i].name);
        }

        for (std::size_t i = 0; i < message.fields.size(); ++i) {
            add(names.defaults[i], message.fields[i].name_span,
                std::format("{}'s default", message.fields[i].name));
        }
    }
};

/// A message's or enum's name after those of the messages it's in, with '_' between.
std::string nested_name(const proto_file_t& file, const std::optional<std::size_t> parent,
                        const std::string_view name) {
    std::string joined_name(name);
    for (std::optional<std::size_t> at = parent; at; at = file.messages[*at].parent) {
        joined_name = std::format("{}_{}", file.messages[*at].name, joined_name);
    }

    return fb_name(std::move(joined_name));
}

} // namespace

void resolve_proto(proto_file_t& file, diagnostic_bag_t& diagnostics) {
    proto_checker_t(file, diagnostics).run();
}

std::string proto_message_name(const proto_file_t& file, const std::size_t index) {
    const proto_message_t& message = file.messages[index];
    return nested_name(file, message.parent, message.name);
}

std::string proto_enum_name(const proto_file_t& file, const std::size_t index) {
    const proto_enum_t& declared = file.enums[index];
    return nested_name(file, declared.parent, declared.name);
}

namespace {

/// The wire types, the low 3 bits of a tag.
constexpr std::uint32_t wire_varint = 0;
constexpr std::uint32_t wire_fixed64 = 1;
constexpr std::uint32_t wire_length = 2;
constexpr std::uint32_t wire_group_start = 3;
constexpr std::uint32_t wire_group_end = 4;
constexpr std::uint32_t wire_fixed32 = 5;

std::uint32_t tag_of(const std::int32_t number, const std::uint32_t wire) {
    return (static_cast<std::uint32_t>(number) << 3) | wire;
}

/// The wire type a value of this kind takes, alone.
std::uint32_t wire_of(const proto_kind_t kind) {
    switch (kind) {
    case proto_kind_t::DOUBLE:
    case proto_kind_t::FIXED64:
    case proto_kind_t::SFIXED64:
        return wire_fixed64;
    case proto_kind_t::FLOAT:
    case proto_kind_t::FIXED32:
    case proto_kind_t::SFIXED32:
        return wire_fixed32;
    case proto_kind_t::STRING:
    case proto_kind_t::BYTES:
    case proto_kind_t::MESSAGE:
    case proto_kind_t::NAMED:
        return wire_length;
    default:
        return wire_varint;
    }
}

/// How many bytes each of a packed field's values takes: 4 or 8, or 0 for varints.
int packed_size(const proto_kind_t kind) {
    switch (wire_of(kind)) {
    case wire_fixed32:
        return 4;
    case wire_fixed64:
        return 8;
    default:
        return 0;
    }
}

/// What ProtoWriter's and ProtoReader's Methods for a kind are called after Write and Read.
std::string_view wire_method(const proto_kind_t kind) {
    switch (kind) {
    case proto_kind_t::DOUBLE:
        return "Double";
    case proto_kind_t::FLOAT:
        return "Float";
    case proto_kind_t::INT64:
        return "Int64";
    case proto_kind_t::UINT32:
        return "UInt32";
    case proto_kind_t::UINT64:
        return "UInt64";
    case proto_kind_t::SINT32:
        return "SInt32";
    case proto_kind_t::SINT64:
        return "SInt64";
    case proto_kind_t::FIXED32:
        return "Fixed32";
    case proto_kind_t::FIXED64:
        return "Fixed64";
    case proto_kind_t::SFIXED32:
        return "SFixed32";
    case proto_kind_t::SFIXED64:
        return "SFixed64";
    case proto_kind_t::BOOL:
        return "Bool";
    case proto_kind_t::STRING:
        return "String";
    case proto_kind_t::BYTES:
        return "Bytes";
    default:
        return "Int32";
    }
}

/// A scalar's FutureBlitz type.
std::string_view scalar_type(const proto_kind_t kind) {
    switch (kind) {
    case proto_kind_t::DOUBLE:
        return "Float64";
    case proto_kind_t::FLOAT:
        return "Float32";
    case proto_kind_t::INT64:
    case proto_kind_t::SINT64:
    case proto_kind_t::SFIXED64:
        return "Int64";
    case proto_kind_t::UINT32:
    case proto_kind_t::FIXED32:
        return "UInt32";
    case proto_kind_t::UINT64:
    case proto_kind_t::FIXED64:
        return "UInt64";
    case proto_kind_t::BOOL:
        return "Bool";
    case proto_kind_t::STRING:
        return "String";
    case proto_kind_t::BYTES:
        return "UInt8[]";
    default:
        return "Int32";
    }
}

/// A scalar's zero, as FutureBlitz writes it.
std::string_view scalar_zero(const proto_kind_t kind) {
    switch (kind) {
    case proto_kind_t::DOUBLE:
    case proto_kind_t::FLOAT:
        return "0.0";
    case proto_kind_t::BOOL:
        return "False";
    case proto_kind_t::STRING:
        return "\"\"";
    case proto_kind_t::BYTES:
        return "[]";
    default:
        return "0";
    }
}

/// A string as a FutureBlitz string literal: in quotes, with escapes for quotes, backslashes,
/// braces, and control characters.
std::string string_literal(const std::string_view text) {
    std::string literal = "\"";
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        switch (c) {
        case '"':
            literal += "\\\"";
            break;
        case '\\':
            literal += "\\\\";
            break;
        case '{':
            literal += "{{";
            break;
        case '}':
            literal += "}}";
            break;
        case '\n':
            literal += "\\n";
            break;
        case '\r':
            literal += "\\r";
            break;
        case '\t':
            literal += "\\t";
            break;
        default:
            if (byte < 0x20 || byte == 0x7F) {
                literal += std::format("\\u{{{:X}}}", byte);
            } else {
                literal += c;
            }
        }
    }

    return literal + "\"";
}

/// Writes FutureBlitz source a line at a time, indented by four spaces a level.
class source_writer_t {
public:
    void line(const std::string_view text = {}) {
        if (!text.empty()) {
            m_text.append(static_cast<std::size_t>(m_depth) * 4, ' ');
            m_text += text;
        }

        m_text += '\n';
    }

    template <typename... Args>
    void line(std::format_string<Args...> fmt, Args&&... args) {
        line(std::string_view(std::format(fmt, std::forward<Args>(args)...)));
    }

    void indent() {
        ++m_depth;
    }

    void dedent() {
        --m_depth;
    }

    std::string take() {
        return std::move(m_text);
    }

private:
    std::string m_text;
    int m_depth = 0;
};

/// The module a .proto file gives, as FutureBlitz source.
class proto_generator_t {
public:
    explicit proto_generator_t(const proto_file_t& file)
        : m_file(file), m_types(module_types(file)) {
        m_taken.insert(file.identifiers.begin(), file.identifiers.end());
        m_taken.insert(m_types.begin(), m_types.end());
        for (const proto_message_t& message : file.messages) {
            const member_names_t names = member_names(message, m_types);
            m_taken.insert(names.fields.begin(), names.fields.end());
            m_taken.insert(names.oneofs.begin(), names.oneofs.end());
            m_taken.insert(names.defaults.begin(), names.defaults.end());
        }

        m_core = fresh("Core_");
        m_collections = fresh("Collections_");
        m_proto = fresh("Proto_");
        m_numbers = fresh("Numbers_");
        // The Locals and parameters of the module's code.
        constexpr std::string_view locals = "writer_ reader_ text_ tag_ group_ item_ outer_ inner_ "
                                            "start_ key_ value_ entry_ bytes_ message_ other_ "
                                            "copy_ prefix_ missing_ number_ known_ index_ given_ "
                                            "request_";
        for (const auto part : std::views::split(locals, ' ')) {
            const std::string name(part.begin(), part.end());
            m_locals.emplace(name, fresh(name));
        }
    }

    std::string run() {
        collect_imports();
        m_out.line("// fb makes this module from {} as it builds the program, so it changes when "
                   "the .proto does.",
                   m_file.path);
        m_out.line("Import std.core As {}", m_core);
        m_out.line("Import std.collections As {}", m_collections);
        m_out.line("Import std.data.protobuf As {}", m_proto);
        if (needs_numbers()) {
            m_out.line("Import std.math.numbers As {}", m_numbers);
        }

        for (const auto& [file, alias] : m_imports) {
            m_out.line("Import {} As {}", string_literal(file->path), alias);
        }

        for (std::size_t i = 0; i < m_file.enums.size(); ++i) {
            m_out.line();
            write_enum(i);
        }

        for (std::size_t i = 0; i < m_file.messages.size(); ++i) {
            for (std::size_t j = 0; j < m_file.messages[i].oneofs.size(); ++j) {
                m_out.line();
                write_oneof_enum(i, j);
            }
        }

        for (std::size_t i = 0; i < m_file.messages.size(); ++i) {
            m_out.line();
            write_message(i);
        }

        for (const proto_service_t& service : m_file.services) {
            m_out.line();
            write_service(service);
        }

        return m_out.take();
    }

private:
    const proto_file_t& m_file;
    std::set<std::string, std::less<>> m_types;
    std::set<std::string, std::less<>> m_taken;
    source_writer_t m_out;
    std::string m_core;
    std::string m_collections;
    std::string m_proto;
    std::string m_numbers;
    std::map<std::string, std::string, std::less<>> m_locals;
    /// The alias of each other file whose types the module names, by path order.
    std::map<const proto_file_t*, std::string> m_imports;
    /// Each file's messages that have required fields, or hold messages that do.
    std::map<const proto_file_t*, std::vector<bool>> m_required;
    /// The current message's members' names, and the Locals that hold its fields' values.
    member_names_t m_names;
    std::vector<std::string> m_values;
    std::vector<bool> m_boxed;
    std::vector<std::string> m_storage;
    std::string m_unknown;

    /// A name none of the file's identifiers or the module's names is: `base`, with '_'s after
    /// it until it's new.
    std::string fresh(std::string base) {
        while (m_taken.contains(base)) {
            base += '_';
        }

        m_taken.insert(base);
        return base;
    }

    [[nodiscard]] const std::string& local(const std::string_view name) const {
        return m_locals.find(name)->second;
    }

    /// The other files whose messages and enums the module names, each with an alias.
    void collect_imports() {
        std::vector<const proto_file_t*> files;
        const auto add = [&](const proto_type_t& type) {
            if (type.file != nullptr && type.file != &m_file &&
                !std::ranges::contains(files, type.file)) {
                files.push_back(type.file);
            }
        };
        for (const proto_message_t& message : m_file.messages) {
            for (const proto_field_t& field : message.fields) {
                add(field.type);
            }
        }

        for (const proto_service_t& service : m_file.services) {
            for (const proto_method_t& method : service.methods) {
                add(method.input);
                add(method.output);
            }
        }

        std::ranges::sort(files, {}, &proto_file_t::path);
        for (const proto_file_t* file : files) {
            std::string_view stem = file->path;
            stem = stem.substr(stem.find_last_of('/') + 1);
            stem = stem.substr(0, stem.find('.'));
            std::string alias(stem);
            std::ranges::replace_if(
                alias, [](const char c) { return !is_letter(c) && !is_digit(c); }, '_');
            if (alias.empty() || is_digit(alias.front())) {
                alias.insert(alias.begin(), 'P');
            }

            m_imports.emplace(file, fresh(alias + "_"));
        }
    }

    /// Whether a float's or double's default needs std.math.numbers' FromBits.
    [[nodiscard]] bool needs_numbers() const {
        return std::ranges::any_of(m_file.messages, [](const proto_message_t& message) {
            return std::ranges::any_of(message.fields, [](const proto_field_t& field) {
                return field.default_value && (field.type.kind == proto_kind_t::FLOAT ||
                                               field.type.kind == proto_kind_t::DOUBLE);
            });
        });
    }

    /// A message's or enum's FutureBlitz type, after its module's alias when it's another
    /// file's.
    [[nodiscard]] std::string named_type(const proto_type_t& type) const {
        const std::string name = type.kind == proto_kind_t::MESSAGE
                                     ? proto_message_name(*type.file, type.index)
                                     : proto_enum_name(*type.file, type.index);
        if (type.file == &m_file) {
            return name;
        }

        return std::format("{}.{}", m_imports.at(type.file), name);
    }

    /// A value's FutureBlitz type.
    [[nodiscard]] std::string value_type(const proto_type_t& type) const {
        if (type.kind == proto_kind_t::MESSAGE || type.kind == proto_kind_t::ENUM) {
            return named_type(type);
        }

        return std::string(scalar_type(type.kind));
    }

    /// The enum a type names.
    [[nodiscard]] static const proto_enum_t& enum_of(const proto_type_t& type) {
        return type.file->enums[type.index];
    }

    /// Whether an enum is open, as proto3's are: it holds numbers it doesn't name.
    [[nodiscard]] static bool is_open(const proto_type_t& type) {
        return type.kind == proto_kind_t::ENUM && type.file->syntax == proto_syntax_t::PROTO3;
    }

    /// The name of a closed enum's Static Method that gives the value of a number, or Null: one
    /// none of its values has.
    [[nodiscard]] static std::string from_number(const proto_enum_t& declared) {
        std::set<std::string, std::less<>> values;
        for (const proto_enum_value_t& value : declared.values) {
            values.insert(value_name(value));
        }

        return fb_name("FromNumber", values);
    }

    /// An enum's first value, its zero, as FutureBlitz writes it.
    [[nodiscard]] std::string enum_zero(const proto_type_t& type) const {
        return std::format("{}.{}", named_type(type), value_name(enum_of(type).values.front()));
    }

    /// A value's zero, for a field without presence.
    [[nodiscard]] std::string zero_of(const proto_type_t& type) const {
        return type.kind == proto_kind_t::ENUM ? enum_zero(type)
                                               : std::string(scalar_zero(type.kind));
    }

    // Enums.

    void write_enum(const std::size_t index) {
        const proto_enum_t& declared = m_file.enums[index];
        const std::string name = proto_enum_name(m_file, index);
        const bool open = m_file.syntax == proto_syntax_t::PROTO3;
        m_out.line("// The enum {} in {}{}.", declared.full_name, m_file.path,
                   open ? ", open: it keeps numbers it doesn't name" : "");
        m_out.line("{}Public Enum {}: Int32", open ? "@Open " : "", name);
        m_out.indent();
        std::vector<const proto_enum_value_t*> aliases;
        std::vector<std::int32_t> numbers;
        for (const proto_enum_value_t& value : declared.values) {
            if (std::ranges::contains(numbers, value.number)) {
                aliases.push_back(&value);
                continue;
            }

            numbers.push_back(value.number);
            m_out.line("{} = {}", value_name(value), value.number);
        }

        m_out.dedent();
        m_out.line("End Enum");
        if (aliases.empty() && open) {
            return;
        }

        m_out.line();
        m_out.line("Extend {}", name);
        m_out.indent();
        for (const proto_enum_value_t* alias : aliases) {
            const auto first =
                std::ranges::find(declared.values, alias->number, &proto_enum_value_t::number);
            m_out.line("// The same value as {}.", value_name(*first));
            m_out.line("Public Static Property {}: {}", value_name(*alias), name);
            m_out.indent();
            m_out.line("Get => {}.{}", name, value_name(*first));
            m_out.dedent();
            m_out.line("End Property");
            m_out.line();
        }

        if (!open) {
            // A closed enum's number that it doesn't name goes in the message's unknown fields.
            m_out.line("// The value whose number this is, or Null when it's none of them.");
            m_out.line("Public Static Method {}(number: Int32): {}?", from_number(declared), name);
            m_out.indent();
            m_out.line("Select number");
            m_out.indent();
            std::string listed;
            for (const std::int32_t number : numbers) {
                listed += std::format("{}{}", listed.empty() ? "" : ", ", number);
            }

            m_out.line("Case {}", listed);
            m_out.indent();
            m_out.line("Return number As {}", name);
            m_out.dedent();
            m_out.line("Default");
            m_out.indent();
            m_out.line("Return Null");
            m_out.dedent();
            m_out.dedent();
            m_out.line("End Select");
            m_out.dedent();
            m_out.line("End Method");
        }

        m_out.dedent();
        m_out.line("End Extend");
    }

    /// The Enum that says which of a oneof's fields holds a value, by its number, or NONE.
    void write_oneof_enum(const std::size_t message_index, const std::size_t oneof) {
        const proto_message_t& message = m_file.messages[message_index];
        const member_names_t names = member_names(message, m_types);
        m_out.line("// Which of {}'s oneof {}'s fields holds a value.", message.full_name,
                   message.oneofs[oneof].name);
        m_out.line("Public Enum {}: Int32", oneof_enum_name(m_file, message_index, oneof));
        m_out.indent();
        m_out.line("{} = 0", oneof_none(names, message, oneof));
        for (std::size_t i = 0; i < message.fields.size(); ++i) {
            if (message.fields[i].oneof == oneof) {
                m_out.line("{} = {}", names.fields[i], message.fields[i].number);
            }
        }

        m_out.dedent();
        m_out.line("End Enum");
    }

    // Messages.

    /// Each file's messages that have a required field, or hold a message that does, which
    /// IsInitialized checks.
    const std::vector<bool>& required(const proto_file_t& file) {
        if (const auto found = m_required.find(&file); found != m_required.end()) {
            return found->second;
        }

        std::vector<bool> marks(file.messages.size());
        for (std::size_t i = 0; i < file.messages.size(); ++i) {
            marks[i] = std::ranges::any_of(file.messages[i].fields, [](const proto_field_t& field) {
                return field.label == proto_label_t::REQUIRED;
            });
        }

        // A message holding one that has to be checked has to be checked too, round any cycles.
        bool changed = true;
        while (changed) {
            changed = false;
            for (std::size_t i = 0; i < file.messages.size(); ++i) {
                if (marks[i]) {
                    continue;
                }

                for (const proto_field_t& field : file.messages[i].fields) {
                    if (field.type.kind != proto_kind_t::MESSAGE) {
                        continue;
                    }

                    const bool checked = field.type.file == &file
                                             ? marks[field.type.index]
                                             : required(*field.type.file)[field.type.index];
                    if (checked) {
                        marks[i] = true;
                        changed = true;
                        break;
                    }
                }
            }
        }

        return m_required.emplace(&file, std::move(marks)).first->second;
    }

    [[nodiscard]] bool checks(const proto_type_t& type) {
        return type.kind == proto_kind_t::MESSAGE && required(*type.file)[type.index];
    }

    /// Whether a field holds one message, not repeated or a map.
    [[nodiscard]] static bool is_singular_message(const proto_field_t& field) {
        return field.type.kind == proto_kind_t::MESSAGE && field.label != proto_label_t::REPEATED;
    }

    /// Whether a field of a message has to hold its message in a ProtoBox: one that holds,
    /// through messages held one each, the message it's in, which a Struct can't.
    [[nodiscard]] bool must_box(const std::size_t message, const proto_field_t& field) const {
        if (!is_singular_message(field) || field.type.file != &m_file) {
            return false;
        }

        std::vector<bool> seen(m_file.messages.size());
        std::vector<std::size_t> pending{ field.type.index };
        while (!pending.empty()) {
            const std::size_t at = pending.back();
            pending.pop_back();
            if (at == message) {
                return true;
            }

            if (seen[at]) {
                continue;
            }

            seen[at] = true;
            for (const proto_field_t& inner : m_file.messages[at].fields) {
                if (is_singular_message(inner) && inner.type.file == &m_file) {
                    pending.push_back(inner.type.index);
                }
            }
        }

        return false;
    }

    /// Whether a field has presence, so it's a T?: optional, required, a message, or a oneof's.
    [[nodiscard]] static bool has_presence(const proto_field_t& field) {
        return field.label != proto_label_t::REPEATED &&
               (field.label != proto_label_t::NONE || field.oneof ||
                field.type.kind == proto_kind_t::MESSAGE);
    }

    /// A field's FutureBlitz type, as its Field or Property has it.
    [[nodiscard]] std::string field_type(const proto_field_t& field) const {
        if (field.key) {
            return std::format("{}.Map<{}, {}>", m_collections, value_type(*field.key),
                               value_type(field.type));
        }

        if (field.label == proto_label_t::REPEATED) {
            return value_type(field.type) + "[]";
        }

        return value_type(field.type) + (has_presence(field) ? "?" : "");
    }

    /// Whether a field is a Property, over private storage: a oneof's, or a boxed one.
    [[nodiscard]] bool is_property(const std::size_t index) const {
        return !m_storage[index].empty();
    }

    /// What reads a field's value in the Struct's code: its storage, for a oneof's field that
    /// isn't boxed, or else the field.
    [[nodiscard]] std::string read_field(const std::size_t index) const {
        return std::format("Self.{}", m_names.fields[index]);
    }

    void write_message(const std::size_t index) {
        const proto_message_t& message = m_file.messages[index];
        const std::string name = proto_message_name(m_file, index);
        m_names = member_names(message, m_types);
        m_boxed.assign(message.fields.size(), false);
        m_storage.assign(message.fields.size(), std::string());
        m_values.clear();
        for (std::size_t i = 0; i < message.fields.size(); ++i) {
            m_boxed[i] = must_box(index, message.fields[i]);
            if (m_boxed[i] || message.fields[i].oneof) {
                m_storage[i] = fresh(m_names.fields[i] + "_");
            }

            m_values.push_back(fresh(m_names.fields[i] + "_value"));
        }

        m_unknown = fresh("unknown_");
        m_out.line("// The message {} in {}.", message.full_name, m_file.path);
        m_out.line("Public Struct {} Implements {}.Equatable, {}.Printable, {}.Cloneable", name,
                   m_core, m_core, m_core);
        m_out.indent();
        write_fields(message);
        m_out.line();
        write_new(message);
        write_properties(message);
        write_encode(message, name, index);
        write_write_to(message);
        write_read_from(message);
        write_merge_from(message, name);
        write_clone(name);
        write_checks(message, index);
        write_text(message);
        m_out.dedent();
        m_out.line("End Struct");
    }

    void write_fields(const proto_message_t& message) {
        for (std::size_t i = 0; i < message.fields.size(); ++i) {
            if (!is_property(i)) {
                m_out.line("{}Public Field {}: {}",
                           message.fields[i].deprecated ? "@Deprecated " : "", m_names.fields[i],
                           field_type(message.fields[i]));
            }
        }

        for (std::size_t i = 0; i < message.fields.size(); ++i) {
            if (is_property(i)) {
                const std::string type = value_type(message.fields[i].type);
                m_out.line("Field {}: {}?", m_storage[i],
                           m_boxed[i] ? std::format("{}.ProtoBox<{}>", m_proto, type) : type);
            }
        }

        // The fields it didn't know, as they were written, which it writes back after its own.
        m_out.line("Field {}: UInt8[]", m_unknown);
    }

    /// A New that takes each field, by name, with a default: its zero, an empty array or Map, or
    /// Null.
    void write_new(const proto_message_t& message) {
        std::string parameters;
        for (std::size_t i = 0; i < message.fields.size(); ++i) {
            const proto_field_t& field = message.fields[i];
            std::string parameter = std::format("{}: {}", m_names.fields[i], field_type(field));
            if (field.key) {
                parameter += "? = Null";
            } else if (field.label == proto_label_t::REPEATED) {
                parameter += " = []";
            } else if (has_presence(field)) {
                parameter += " = Null";
            } else {
                parameter += " = " + zero_of(field.type);
            }

            parameters += (parameters.empty() ? "" : ", ") + parameter;
        }

        const bool oneofs = !message.oneofs.empty();
        if (oneofs) {
            m_out.line("@PanicsAtCaller");
        }

        m_out.line("Public New({})", parameters);
        m_out.indent();
        for (std::size_t i = 0; i < message.fields.size(); ++i) {
            if (is_property(i)) {
                continue;
            }

            m_out.line("Self.{} = {}{}", m_names.fields[i], m_names.fields[i],
                       message.fields[i].key ? " ?? {}" : "");
        }

        m_out.line("Self.{} = []", m_unknown);
        if (!message.oneofs.empty()) {
            m_out.line("Local {} = 0", local("given_"));
        }

        for (std::size_t j = 0; j < message.oneofs.size(); ++j) {
            std::vector<std::size_t> members;
            for (std::size_t i = 0; i < message.fields.size(); ++i) {
                if (message.fields[i].oneof == j) {
                    members.push_back(i);
                }
            }

            if (members.size() < 2) {
                continue;
            }

            m_out.line("{} = 0", local("given_"));
            std::string listed;
            for (std::size_t k = 0; k < members.size(); ++k) {
                const std::string& member = m_names.fields[members[k]];
                m_out.line("If {} != Null Then {} += 1", member, local("given_"));
                std::string_view between = k == 0 ? "" : ", ";
                if (k > 0 && k + 1 == members.size()) {
                    between = members.size() > 2 ? ", or " : " or ";
                }

                listed += std::format("{}{}", between, member);
            }

            // "Person's oneof contact holds one of phone or friend, not {given_}".
            std::string text =
                string_literal(std::format("{}'s oneof {} holds one of {}, not ", message.name,
                                           message.oneofs[j].name, listed));
            text.pop_back();
            text += std::format("{{{}}}\"", local("given_"));
            m_out.line("If {} > 1 Then {}.ProtoPanic({})", local("given_"), m_proto, text);
        }

        for (std::size_t i = 0; i < message.fields.size(); ++i) {
            if (is_property(i)) {
                m_out.line("Self.{} = {}", m_names.fields[i], m_names.fields[i]);
            }
        }

        m_out.dedent();
        m_out.line("End New");
    }

    /// The Properties over a oneof's fields' and the boxed fields' storage, each oneof's Property
    /// of which of its fields holds a value, and the defaults' Properties.
    void write_properties(const proto_message_t& message) {
        for (std::size_t i = 0; i < message.fields.size(); ++i) {
            if (!is_property(i)) {
                continue;
            }

            const proto_field_t& field = message.fields[i];
            const std::string type = value_type(field.type);
            m_out.line();
            if (field.oneof) {
                m_out.line("// One of oneof {}'s fields: setting it to a value takes the others' "
                           "away.",
                           message.oneofs[*field.oneof].name);
            }

            m_out.line("{}Public Property {}: {}?", field.deprecated ? "@Deprecated " : "",
                       m_names.fields[i], type);
            m_out.indent();
            m_out.line("Get => Self.{}{}", m_storage[i], m_boxed[i] ? "?.Value" : "");
            m_out.line("Set(value)");
            m_out.indent();
            if (field.oneof) {
                for (std::size_t j = 0; j < message.fields.size(); ++j) {
                    if (j != i && message.fields[j].oneof == field.oneof) {
                        m_out.line("If value != Null Then Self.{} = Null", m_storage[j]);
                    }
                }
            }

            if (m_boxed[i]) {
                m_out.line("Self.{} = value == Null ? Null : {}.ProtoBox<{}>(value)", m_storage[i],
                           m_proto, type);
            } else {
                m_out.line("Self.{} = value", m_storage[i]);
            }

            m_out.dedent();
            m_out.line("End Set");
            m_out.dedent();
            m_out.line("End Property");
        }

        for (std::size_t j = 0; j < message.oneofs.size(); ++j) {
            const std::string kind = oneof_enum_name(m_file, index_of(message), j);
            m_out.line();
            m_out.line("// Which of oneof {}'s fields holds a value.", message.oneofs[j].name);
            m_out.line("Public Property {}: {}", m_names.oneofs[j], kind);
            m_out.indent();
            m_out.line("Get");
            m_out.indent();
            for (std::size_t i = 0; i < message.fields.size(); ++i) {
                if (message.fields[i].oneof == j) {
                    m_out.line("If Self.{} != Null Then Return {}.{}", m_storage[i], kind,
                               m_names.fields[i]);
                }
            }

            m_out.line("Return {}.{}", kind, oneof_none(m_names, message, j));
            m_out.dedent();
            m_out.line("End Get");
            m_out.dedent();
            m_out.line("End Property");
        }

        for (std::size_t i = 0; i < message.fields.size(); ++i) {
            if (m_names.defaults[i].empty()) {
                continue;
            }

            const proto_field_t& field = message.fields[i];
            m_out.line();
            m_out.line("// {}'s value, or its default when it has none.", m_names.fields[i]);
            m_out.line("Public Property {}: {}", m_names.defaults[i], value_type(field.type));
            m_out.indent();
            m_out.line("Get => Self.{} ?? {}", m_names.fields[i], default_value(field));
            m_out.dedent();
            m_out.line("End Property");
        }
    }

    [[nodiscard]] std::size_t index_of(const proto_message_t& message) const {
        return static_cast<std::size_t>(&message - m_file.messages.data());
    }

    /// A field's [default = ...] as FutureBlitz writes its value.
    [[nodiscard]] std::string default_value(const proto_field_t& field) const {
        const proto_constant_t& value = *field.default_value;
        switch (field.type.kind) {
        case proto_kind_t::BOOL:
            return value.text == "true" ? "True" : "False";
        case proto_kind_t::STRING:
            return string_literal(value.text);
        case proto_kind_t::BYTES: {
            std::string bytes;
            for (const char c : value.text) {
                bytes +=
                    std::format("{}{}", bytes.empty() ? "" : ", ", static_cast<unsigned char>(c));
            }

            return std::format("[{}]", bytes);
        }
        case proto_kind_t::ENUM: {
            const proto_enum_t& declared = enum_of(field.type);
            const auto found =
                std::ranges::find(declared.values, value.text, &proto_enum_value_t::name);
            return std::format("{}.{}", named_type(field.type), value_name(*found));
        }
        case proto_kind_t::FLOAT:
        case proto_kind_t::DOUBLE:
            return float_default(field.type.kind, value);
        default:
            break;
        }

        // An integer: one too big for an Int64 as a UInt64, and the lowest Int64 as one less
        // than the next, which a literal can't write.
        if (value.negative && value.integer == 0) {
            return "0";
        }

        return std::format("{}{}", value.negative ? "-" : "", value.integer);
    }

    /// A float's or double's default, exactly, as its bits.
    [[nodiscard]] static std::string float_default(const proto_kind_t kind,
                                                   const proto_constant_t& value) {
        double number = value.number;
        if (value.kind == proto_constant_kind_t::INTEGER) {
            number = static_cast<double>(value.integer);
            if (value.negative) {
                number = -number;
            }
        } else if (value.kind == proto_constant_kind_t::IDENTIFIER) {
            number = value.text == "inf" ? std::numeric_limits<double>::infinity()
                                         : std::numeric_limits<double>::quiet_NaN();
        }

        if (kind == proto_kind_t::FLOAT) {
            return std::format("Float32.FromBits(0x{:08X})",
                               std::bit_cast<std::uint32_t>(static_cast<float>(number)));
        }

        return std::format("Float64.FromBits(0x{:016X})", std::bit_cast<std::uint64_t>(number));
    }

    void write_encode(const proto_message_t& message, const std::string& name,
                      const std::size_t index) {
        const bool checked = required(m_file)[index];
        m_out.line();
        m_out.line("// Its bytes in protobuf's wire format.{}",
                   checked ? " Panics when a required field has no value." : "");
        if (checked) {
            m_out.line("@PanicsAtCaller");
        }

        m_out.line("Public Method Encode(): UInt8[]");
        m_out.indent();
        if (checked) {
            write_missing_check(message, true);
        }

        m_out.line("Local {} = {}.ProtoWriter()", local("writer_"), m_proto);
        m_out.line("Self.WriteTo({})", local("writer_"));
        m_out.line("Return {}.Finish()", local("writer_"));
        m_out.dedent();
        m_out.line("End Method");
        m_out.line();
        m_out.line("// The {} in bytes of protobuf's wire format. Throws a ParseError when they "
                   "don't read{}.",
                   name, checked ? ", or a required field has no value" : "");
        m_out.line("Public Static Method Decode({}: UInt8[]): {} Throws {}.ParseError",
                   local("bytes_"), name, m_core);
        m_out.indent();
        m_out.line("Local {} = {}()", local("message_"), name);
        m_out.line("Try {}.Merge({})", local("message_"), local("bytes_"));
        m_out.line("Return {}", local("message_"));
        m_out.dedent();
        m_out.line("End Method");
        m_out.line();
        m_out.line("// Reads bytes of protobuf's wire format into it, as protobuf merges: a field "
                   "that holds one value takes the bytes', a message's merges, and a repeated "
                   "one's go after its own.");
        m_out.line("Public ByRef Method Merge({}: UInt8[]) Throws {}.ParseError", local("bytes_"),
                   m_core);
        m_out.indent();
        m_out.line("Try Self.ReadFrom({}.ProtoReader({}), 0)", m_proto, local("bytes_"));
        if (checked) {
            write_missing_check(message, false);
        }

        m_out.dedent();
        m_out.line("End Method");
    }

    /// What happens when a required field has no value: Encode panics, and Merge throws a
    /// ParseError, saying which.
    void write_missing_check(const proto_message_t& message, const bool panic) {
        const std::string& missing = local("missing_");
        m_out.line("If !Self.IsInitialized");
        m_out.indent();
        m_out.line("Local {}: String[] = []", missing);
        m_out.line("Self.FindMissing(\"\", {})", missing);
        // "Person is missing required fields: {missing_.Join(", ")}".
        const std::string text = std::format(
            R"("{} is missing required fields: {{{}.Join(", ")}}")", message.name, missing);
        if (panic) {
            m_out.line("{}.ProtoPanic({})", m_proto, text);
        } else {
            m_out.line("Throw {}.ParseError({}.ParseErrorKind.OTHER, {})", m_core, m_core, text);
        }

        m_out.dedent();
        m_out.line("End If");
        m_out.line();
    }

    /// The fields in the order protobuf writes them: by number.
    [[nodiscard]] static std::vector<std::size_t> by_number(const proto_message_t& message) {
        std::vector<std::size_t> order(message.fields.size());
        for (std::size_t i = 0; i < order.size(); ++i) {
            order[i] = i;
        }

        std::ranges::sort(order, {}, [&](const std::size_t i) { return message.fields[i].number; });
        return order;
    }

    /// What a field without presence has when it's not its zero, which proto3 doesn't write.
    [[nodiscard]] std::string not_zero(const proto_type_t& type, const std::string& value) const {
        switch (type.kind) {
        case proto_kind_t::FLOAT:
        case proto_kind_t::DOUBLE:
            return std::format("!{}.ProtoIsZero({})", m_proto, value);
        case proto_kind_t::BOOL:
            return value;
        case proto_kind_t::STRING:
            return std::format("{} != \"\"", value);
        case proto_kind_t::BYTES:
            return std::format("{}.Count > 0", value);
        case proto_kind_t::ENUM:
            return std::format("({} As Int32) != 0", value);
        default:
            return std::format("{} != 0", value);
        }
    }

    /// Writes one value, its tag first, with the writer.
    void write_value(const proto_field_t& field, const proto_type_t& type, const std::string& value,
                     const std::int32_t number) {
        const std::string& writer = local("writer_");
        if (field.group && &type == &field.type) {
            m_out.line("{}.WriteTag({})", writer, tag_of(number, wire_group_start));
            m_out.line("{}.WriteTo({})", value, writer);
            m_out.line("{}.WriteTag({})", writer, tag_of(number, wire_group_end));
            return;
        }

        m_out.line("{}.WriteTag({})", writer, tag_of(number, wire_of(type.kind)));
        write_bare(type, value);
    }

    /// Writes a value without its tag: a message between its length's start and end.
    void write_bare(const proto_type_t& type, const std::string& value) {
        const std::string& writer = local("writer_");
        if (type.kind == proto_kind_t::MESSAGE) {
            m_out.line("Local {} = {}.BeginLength()", local("start_"), writer);
            m_out.line("{}.WriteTo({})", value, writer);
            m_out.line("{}.EndLength({})", writer, local("start_"));
        } else if (type.kind == proto_kind_t::ENUM) {
            m_out.line("{}.WriteInt32({} As Int32)", writer, value);
        } else {
            m_out.line("{}.Write{}({})", writer, wire_method(type.kind), value);
        }
    }

    /// Whether a repeated field is written packed: proto3's numbers, bools, and enums, unless
    /// [packed = false], and proto2's with [packed = true].
    [[nodiscard]] bool is_packed(const proto_field_t& field) const {
        if (field.label != proto_label_t::REPEATED || field.key || !is_packable(field.type.kind)) {
            return false;
        }

        return field.packed.value_or(m_file.syntax == proto_syntax_t::PROTO3);
    }

    /// WriteTo: each field with a value, by number, then the unknown ones.
    void write_write_to(const proto_message_t& message) {
        const std::string& writer = local("writer_");
        m_out.line();
        m_out.line("// Writes it with a ProtoWriter, as a message inside another is written.");
        m_out.line("Public Method WriteTo({}: {}.ProtoWriter)", writer, m_proto);
        m_out.indent();
        for (const std::size_t i : by_number(message)) {
            write_field(message.fields[i], i);
            m_out.line();
        }

        m_out.line("{}.WriteRaw(Self.{})", writer, m_unknown);
        m_out.dedent();
        m_out.line("End Method");
    }

    void write_field(const proto_field_t& field, const std::size_t index) {
        const std::string& item = local("item_");
        if (field.key) {
            m_out.line("For {}, {} In {}", local("key_"), local("value_"), read_field(index));
            m_out.indent();
            m_out.line("{}.WriteTag({})", local("writer_"), tag_of(field.number, wire_length));
            m_out.line("Local {} = {}.BeginLength()", local("outer_"), local("writer_"));
            m_out.line("{}.WriteTag({})", local("writer_"), tag_of(1, wire_of(field.key->kind)));
            write_bare(*field.key, local("key_"));
            m_out.line("{}.WriteTag({})", local("writer_"), tag_of(2, wire_of(field.type.kind)));
            write_bare(field.type, local("value_"));
            m_out.line("{}.EndLength({})", local("writer_"), local("outer_"));
            m_out.dedent();
            m_out.line("End For");
            return;
        }

        if (is_packed(field)) {
            m_out.line("If {}.Count > 0", read_field(index));
            m_out.indent();
            m_out.line("{}.WriteTag({})", local("writer_"), tag_of(field.number, wire_length));
            m_out.line("Local {} = {}.BeginLength()", local("outer_"), local("writer_"));
            m_out.line("For {} In {}", item, read_field(index));
            m_out.indent();
            write_bare(field.type, item);
            m_out.dedent();
            m_out.line("End For");
            m_out.line("{}.EndLength({})", local("writer_"), local("outer_"));
            m_out.dedent();
            m_out.line("End If");
            return;
        }

        if (field.label == proto_label_t::REPEATED) {
            m_out.line("For {} In {}", item, read_field(index));
            m_out.indent();
            write_value(field, field.type, item, field.number);
            m_out.dedent();
            m_out.line("End For");
            return;
        }

        if (!has_presence(field)) {
            m_out.line("If {}", not_zero(field.type, read_field(index)));
            m_out.indent();
            write_value(field, field.type, read_field(index), field.number);
            m_out.dedent();
            m_out.line("End If");
            return;
        }

        const std::string& value = m_values[index];
        m_out.line("Local {} = {}", value, read_field(index));
        m_out.line("If {} != Null", value);
        m_out.indent();
        write_value(field, field.type, value, field.number);
        m_out.dedent();
        m_out.line("End If");
    }

    /// ReadFrom: each field's value by its tag, which a field it doesn't know keeps, as written.
    void write_read_from(const proto_message_t& message) {
        const std::string& reader = local("reader_");
        const std::string& tag = local("tag_");
        m_out.line();
        m_out.line("// Reads its fields with a ProtoReader, as a message inside another is read, "
                   "up to the end of what the reader reads, or for a group, up to the end tag "
                   "`{}`, which is 0 for a message.",
                   local("group_"));
        m_out.line("Public ByRef Method ReadFrom({}: {}.ProtoReader, {}: UInt32) Throws "
                   "{}.ParseError",
                   reader, m_proto, local("group_"), m_core);
        m_out.indent();
        m_out.line("While Try {}.More({})", reader, local("group_"));
        m_out.indent();
        m_out.line("Local {} = Try {}.ReadTag()", tag, reader);
        m_out.line("If {} == {} Then Return", tag, local("group_"));
        m_out.line("Select {}", tag);
        m_out.indent();
        for (std::size_t i = 0; i < message.fields.size(); ++i) {
            read_field_cases(message.fields[i], i);
        }

        m_out.line("Default");
        m_out.indent();
        m_out.line("Try {}.Skip({}, Self.{})", reader, tag, m_unknown);
        m_out.dedent();
        m_out.dedent();
        m_out.line("End Select");
        m_out.dedent();
        m_out.line("End While");
        m_out.dedent();
        m_out.line("End Method");
    }

    /// An expression that reads a scalar value, or an open enum's.
    [[nodiscard]] std::string read_scalar(const proto_type_t& type) const {
        if (type.kind == proto_kind_t::ENUM) {
            return std::format("(Try {}.ReadInt32()) As {}", local("reader_"), named_type(type));
        }

        return std::format("Try {}.Read{}()", local("reader_"), wire_method(type.kind));
    }

    /// Reads a message into `target`, a Local, after its tag: its length, then its fields.
    void read_message(const std::string& target, const std::string& outer) {
        m_out.line("Local {} = Try {}.BeginMessage()", outer, local("reader_"));
        m_out.line("Try {}.ReadFrom({}, 0)", target, local("reader_"));
        m_out.line("{}.EndMessage({})", local("reader_"), outer);
    }

    /// Stores a value read into the field: adding it to a repeated one.
    void store(const proto_field_t& field, const std::size_t index, const std::string& value) {
        if (field.label == proto_label_t::REPEATED) {
            m_out.line("{}.Add({})", read_field(index), value);
        } else {
            m_out.line("{} = {}", read_field(index), value);
        }
    }

    /// Reads a closed enum's number into the field, or keeps it as an unknown field.
    void read_closed_enum(const proto_field_t& field, const std::size_t index) {
        const proto_enum_t& declared = enum_of(field.type);
        m_out.line("Local {} = Try {}.ReadInt32()", local("number_"), local("reader_"));
        m_out.line("Local {} = {}.{}({})", local("known_"), named_type(field.type),
                   from_number(declared), local("number_"));
        m_out.line("If {} != Null", local("known_"));
        m_out.indent();
        store(field, index, local("known_"));
        m_out.dedent();
        m_out.line("Else");
        m_out.indent();
        m_out.line("{}.ProtoKeepEnum(Self.{}, {}, {})", m_proto, m_unknown,
                   tag_of(field.number, wire_varint), local("number_"));
        m_out.dedent();
        m_out.line("End If");
    }

    void read_field_cases(const proto_field_t& field, const std::size_t index) {
        const std::string& reader = local("reader_");
        if (field.key) {
            read_map(field, index);
            return;
        }

        if (field.group) {
            m_out.line("Case {}", tag_of(field.number, wire_group_start));
            m_out.indent();
            const std::string type = value_type(field.type);
            if (field.label == proto_label_t::REPEATED) {
                m_out.line("Local {} = {}()", local("item_"), type);
            } else {
                m_out.line("Local {} = {} ?? {}()", local("item_"), read_field(index), type);
            }

            m_out.line("Try {}.BeginGroup()", reader);
            m_out.line("Try {}.ReadFrom({}, {})", local("item_"), reader,
                       tag_of(field.number, wire_group_end));
            m_out.line("{}.EndGroup()", reader);
            store(field, index, local("item_"));
            m_out.dedent();
            return;
        }

        if (field.type.kind == proto_kind_t::MESSAGE) {
            m_out.line("Case {}", tag_of(field.number, wire_length));
            m_out.indent();
            const std::string type = value_type(field.type);
            if (field.label == proto_label_t::REPEATED) {
                m_out.line("Local {} = {}()", local("item_"), type);
            } else {
                // A message's field written twice merges, as protobuf does.
                m_out.line("Local {} = {} ?? {}()", local("item_"), read_field(index), type);
            }

            read_message(local("item_"), local("outer_"));
            store(field, index, local("item_"));
            m_out.dedent();
            return;
        }

        const bool closed = field.type.kind == proto_kind_t::ENUM && !is_open(field.type);
        m_out.line("Case {}", tag_of(field.number, wire_of(field.type.kind)));
        m_out.indent();
        if (closed) {
            read_closed_enum(field, index);
        } else {
            store(field, index, read_scalar(field.type));
        }

        m_out.dedent();
        if (field.label != proto_label_t::REPEATED || !is_packable(field.type.kind)) {
            return;
        }

        // A repeated number reads packed or not, whichever the writer chose.
        m_out.line("Case {}", tag_of(field.number, wire_length));
        m_out.indent();
        m_out.line("Local {} = Try {}.BeginPacked({})", local("outer_"), reader,
                   packed_size(field.type.kind));
        m_out.line("While !{}.AtEnd", reader);
        m_out.indent();
        if (closed) {
            read_closed_enum(field, index);
        } else {
            store(field, index, read_scalar(field.type));
        }

        m_out.dedent();
        m_out.line("End While");
        m_out.line("{}.EndPacked({})", reader, local("outer_"));
        m_out.dedent();
    }

    /// A map's entry: a message of the key, field 1, and the value, field 2, either of which can
    /// be left out for its zero. A closed enum's value it doesn't name keeps the whole entry as
    /// an unknown field.
    void read_map(const proto_field_t& field, const std::size_t index) {
        const std::string& reader = local("reader_");
        const proto_type_t& key = *field.key;
        const proto_type_t& value = field.type;
        const bool closed = value.kind == proto_kind_t::ENUM && !is_open(value);
        m_out.line("Case {}", tag_of(field.number, wire_length));
        m_out.indent();
        if (closed) {
            m_out.line("Local {} = {}.TagStart", local("start_"), reader);
        }

        m_out.line("Local {} = Try {}.BeginMessage()", local("outer_"), reader);
        m_out.line("Local {}: {} = {}", local("key_"), value_type(key), zero_of(key));
        if (value.kind == proto_kind_t::MESSAGE) {
            m_out.line("Local {} = {}()", local("value_"), value_type(value));
        } else if (closed) {
            m_out.line("Local {}: Int32 = {} As Int32", local("value_"), enum_zero(value));
        } else {
            m_out.line("Local {}: {} = {}", local("value_"), value_type(value), zero_of(value));
        }

        m_out.line("While Try {}.More(0)", reader);
        m_out.indent();
        m_out.line("Local {} = Try {}.ReadTag()", local("entry_"), reader);
        m_out.line("Select {}", local("entry_"));
        m_out.indent();
        m_out.line("Case {}", tag_of(1, wire_of(key.kind)));
        m_out.indent();
        m_out.line("{} = {}", local("key_"), read_scalar(key));
        m_out.dedent();
        m_out.line("Case {}", tag_of(2, wire_of(value.kind)));
        m_out.indent();
        if (value.kind == proto_kind_t::MESSAGE) {
            read_message(local("value_"), local("inner_"));
        } else if (closed) {
            m_out.line("{} = Try {}.ReadInt32()", local("value_"), reader);
        } else {
            m_out.line("{} = {}", local("value_"), read_scalar(value));
        }

        m_out.dedent();
        m_out.line("Default");
        m_out.indent();
        m_out.line("Try {}.Skip({}, [])", reader, local("entry_"));
        m_out.dedent();
        m_out.dedent();
        m_out.line("End Select");
        m_out.dedent();
        m_out.line("End While");
        m_out.line("{}.EndMessage({})", reader, local("outer_"));
        if (!closed) {
            m_out.line("{}[{}] = {}", read_field(index), local("key_"), local("value_"));
            m_out.dedent();
            return;
        }

        const proto_enum_t& declared = enum_of(value);
        m_out.line("Local {} = {}.{}({})", local("known_"), named_type(value),
                   from_number(declared), local("value_"));
        m_out.line("If {} != Null", local("known_"));
        m_out.indent();
        m_out.line("{}[{}] = {}", read_field(index), local("key_"), local("known_"));
        m_out.dedent();
        m_out.line("Else");
        m_out.indent();
        m_out.line("{}.KeepFrom({}, Self.{})", reader, local("start_"), m_unknown);
        m_out.dedent();
        m_out.line("End If");
        m_out.dedent();
    }

    /// A copy of a value that shares nothing with it: a message's Clone, and an array's for
    /// bytes.
    [[nodiscard]] static std::string copy_of(const proto_type_t& type, const std::string& value) {
        if (type.kind == proto_kind_t::MESSAGE || type.kind == proto_kind_t::BYTES) {
            return value + ".Clone()";
        }

        return value;
    }

    /// MergeFrom: what protobuf's merge does with another message of the type.
    void write_merge_from(const proto_message_t& message, const std::string& name) {
        const std::string& other = local("other_");
        m_out.line();
        m_out.line("// Merges another {} into it, as protobuf does: a field that holds one value "
                   "takes the other's, when it has one, a message's merges, and a repeated "
                   "field's and a map's values are added, copies that share nothing with the "
                   "other.",
                   name);
        m_out.line("Public ByRef Method MergeFrom({}: {})", other, name);
        m_out.indent();
        for (std::size_t i = 0; i < message.fields.size(); ++i) {
            merge_field(message.fields[i], i);
        }

        m_out.line("Self.{}.AddAll({}.{})", m_unknown, other, m_unknown);
        m_out.dedent();
        m_out.line("End Method");
    }

    void merge_field(const proto_field_t& field, const std::size_t index) {
        const std::string theirs = std::format("{}.{}", local("other_"), m_names.fields[index]);
        const std::string& item = local("item_");
        if (field.key) {
            m_out.line("For {}, {} In {}", local("key_"), local("value_"), theirs);
            m_out.indent();
            m_out.line("{}[{}] = {}", read_field(index), local("key_"),
                       copy_of(field.type, local("value_")));
            m_out.dedent();
            m_out.line("End For");
            return;
        }

        if (field.label == proto_label_t::REPEATED) {
            if (field.type.kind == proto_kind_t::MESSAGE ||
                field.type.kind == proto_kind_t::BYTES) {
                m_out.line("For {} In {}.Clone()", item, theirs);
                m_out.indent();
                m_out.line("{}.Add({})", read_field(index), copy_of(field.type, item));
                m_out.dedent();
                m_out.line("End For");
            } else {
                m_out.line("{}.AddAll({})", read_field(index), theirs);
            }

            return;
        }

        if (!has_presence(field)) {
            m_out.line("If {} Then {} = {}", not_zero(field.type, theirs), read_field(index),
                       copy_of(field.type, theirs));
            return;
        }

        const std::string& value = m_values[index];
        m_out.line("Local {} = {}", value, theirs);
        m_out.line("If {} != Null", value);
        m_out.indent();
        if (field.type.kind == proto_kind_t::MESSAGE) {
            m_out.line("Local {} = {} ?? {}()", item, read_field(index), value_type(field.type));
            m_out.line("{}.MergeFrom({})", item, value);
            m_out.line("{} = {}", read_field(index), item);
        } else {
            m_out.line("{} = {}", read_field(index), copy_of(field.type, value));
        }

        m_out.dedent();
        m_out.line("End If");
    }

    void write_clone(const std::string& name) {
        m_out.line();
        m_out.line("// A copy that shares nothing with it.");
        m_out.line("Public Method Clone(): {}", name);
        m_out.indent();
        m_out.line("Local {} = {}()", local("copy_"), name);
        m_out.line("{}.MergeFrom(Self)", local("copy_"));
        m_out.line("Return {}", local("copy_"));
        m_out.dedent();
        m_out.line("End Method");
    }

    /// IsInitialized and FindMissing: whether every required field, in it and in the messages
    /// it holds, has a value, and the paths of those that don't.
    void write_checks(const proto_message_t& message, const std::size_t index) {
        const bool checked = required(m_file)[index];
        m_out.line();
        m_out.line("// Whether each of its required fields, and theirs, has a value.");
        m_out.line("Public Property IsInitialized: Bool");
        m_out.indent();
        if (!checked) {
            m_out.line("Get => True");
            m_out.dedent();
            m_out.line("End Property");
            m_out.line();
            m_out.line("// Adds the path of each required field without a value to `{}`, after "
                       "`{}`.",
                       local("missing_"), local("prefix_"));
            m_out.line("Public Method FindMissing({}: String, {}: String[])", local("prefix_"),
                       local("missing_"));
            m_out.line("End Method");
            return;
        }

        m_out.line("Get");
        m_out.indent();
        for (std::size_t i = 0; i < message.fields.size(); ++i) {
            const proto_field_t& field = message.fields[i];
            if (field.label == proto_label_t::REQUIRED) {
                m_out.line("If {} == Null Then Return False", read_field(i));
            }

            if (!checks(field.type)) {
                continue;
            }

            if (field.key) {
                m_out.line("For {}, {} In {}", local("key_"), local("value_"), read_field(i));
                m_out.indent();
                m_out.line("If !{}.IsInitialized Then Return False", local("value_"));
                m_out.dedent();
                m_out.line("End For");
            } else if (field.label == proto_label_t::REPEATED) {
                m_out.line("For {} In {}", local("item_"), read_field(i));
                m_out.indent();
                m_out.line("If !{}.IsInitialized Then Return False", local("item_"));
                m_out.dedent();
                m_out.line("End For");
            } else {
                m_out.line("Local {} = {}", m_values[i], read_field(i));
                m_out.line("If {} != Null && !{}.IsInitialized Then Return False", m_values[i],
                           m_values[i]);
            }
        }

        m_out.line("Return True");
        m_out.dedent();
        m_out.line("End Get");
        m_out.dedent();
        m_out.line("End Property");
        m_out.line();
        m_out.line("// Adds the path of each required field without a value to `{}`, after `{}`.",
                   local("missing_"), local("prefix_"));
        m_out.line("Public Method FindMissing({}: String, {}: String[])", local("prefix_"),
                   local("missing_"));
        m_out.indent();
        const std::string& prefix = local("prefix_");
        const std::string& missing = local("missing_");
        for (std::size_t i = 0; i < message.fields.size(); ++i) {
            const proto_field_t& field = message.fields[i];
            if (field.label == proto_label_t::REQUIRED) {
                m_out.line("If {} == Null Then {}.Add({} + {})", read_field(i), missing, prefix,
                           string_literal(field.name));
            }

            if (!checks(field.type)) {
                continue;
            }

            if (field.key) {
                m_out.line("For {}, {} In {}", local("key_"), local("value_"), read_field(i));
                m_out.indent();
                m_out.line("{}.FindMissing(\"{{{}}}{}[{{{}}}].\", {})", local("value_"), prefix,
                           field.name, local("key_"), missing);
                m_out.dedent();
                m_out.line("End For");
            } else if (field.label == proto_label_t::REPEATED) {
                m_out.line("For {} = 0 Until {}.Count", local("index_"), read_field(i));
                m_out.indent();
                m_out.line("{}[{}].FindMissing(\"{{{}}}{}[{{{}}}].\", {})", read_field(i),
                           local("index_"), prefix, field.name, local("index_"), missing);
                m_out.dedent();
                m_out.line("End For");
            } else {
                m_out.line("Local {} = {}", m_values[i], read_field(i));
                m_out.line("If {} != Null Then {}.FindMissing({} + {}, {})", m_values[i],
                           m_values[i], prefix, string_literal(field.name + "."), missing);
            }
        }

        m_out.dedent();
        m_out.line("End Method");
    }

    /// Writes a value as text format, with its field's name.
    void text_value(const proto_field_t& field, const proto_type_t& type, const std::string& name,
                    const std::string& value) {
        const std::string& text = local("text_");
        switch (type.kind) {
        case proto_kind_t::MESSAGE:
            m_out.line("{}.BeginMessage({})", text,
                       string_literal(field.group && &type == &field.type
                                          ? field.type.file->messages[field.type.index].name
                                          : name));
            m_out.line("{}.WriteText({})", value, text);
            m_out.line("{}.EndMessage()", text);
            return;
        case proto_kind_t::BOOL:
        case proto_kind_t::FLOAT:
        case proto_kind_t::DOUBLE:
        case proto_kind_t::STRING:
        case proto_kind_t::BYTES:
            m_out.line("{}.Write{}({}, {})", text, wire_method(type.kind), string_literal(name),
                       value);
            return;
        default:
            m_out.line("{}.Write({}, \"{{{}}}\")", text, string_literal(name), value);
            return;
        }
    }

    /// ToString and WriteText: its fields as protobuf's text format writes them, on one line, by
    /// number, then its unknown fields by number.
    void write_text(const proto_message_t& message) {
        const std::string& text = local("text_");
        m_out.line();
        m_out.line("// Its fields as protobuf's text format writes them, on one line.");
        m_out.line("Public Method ToString(): String");
        m_out.indent();
        m_out.line("Local {} = {}.ProtoText()", text, m_proto);
        m_out.line("Self.WriteText({})", text);
        m_out.line("Return {}.Finish()", text);
        m_out.dedent();
        m_out.line("End Method");
        m_out.line();
        m_out.line(
            "// Writes its fields with a ProtoText, as a message inside another is written.");
        m_out.line("Public Method WriteText({}: {}.ProtoText)", text, m_proto);
        m_out.indent();
        for (const std::size_t i : by_number(message)) {
            text_field(message.fields[i], i);
        }

        m_out.line("{}.WriteUnknown(Self.{})", text, m_unknown);
        m_out.dedent();
        m_out.line("End Method");
    }

    void text_field(const proto_field_t& field, const std::size_t index) {
        const std::string& item = local("item_");
        if (field.key) {
            m_out.line("For {}, {} In {}", local("key_"), local("value_"), read_field(index));
            m_out.indent();
            m_out.line("{}.BeginMessage({})", local("text_"), string_literal(field.name));
            text_value(field, *field.key, "key", local("key_"));
            text_value(field, field.type, "value", local("value_"));
            m_out.line("{}.EndMessage()", local("text_"));
            m_out.dedent();
            m_out.line("End For");
            return;
        }

        if (field.label == proto_label_t::REPEATED) {
            m_out.line("For {} In {}", item, read_field(index));
            m_out.indent();
            text_value(field, field.type, field.name, item);
            m_out.dedent();
            m_out.line("End For");
            return;
        }

        if (!has_presence(field)) {
            m_out.line("If {}", not_zero(field.type, read_field(index)));
            m_out.indent();
            text_value(field, field.type, field.name, read_field(index));
            m_out.dedent();
            m_out.line("End If");
            return;
        }

        const std::string& value = m_values[index];
        m_out.line("Local {} = {}", value, read_field(index));
        m_out.line("If {} != Null", value);
        m_out.indent();
        text_value(field, field.type, field.name, value);
        m_out.dedent();
        m_out.line("End If");
    }

    // Services.

    /// An Interface of an Async Method for each rpc, which takes the request and gives the
    /// response, and can throw any Error, as a transport would.
    void write_service(const proto_service_t& service) {
        m_out.line("// The service {} in {}: each rpc is an Async Method, which a server "
                   "implements, and a client calls through its transport.",
                   service.full_name, m_file.path);
        m_out.line("Public Interface {}", service_name(service));
        m_out.indent();
        for (const proto_method_t& method : service.methods) {
            const std::string input = value_type(method.input);
            const std::string output = value_type(method.output);
            m_out.line("{}Async Method {}({}: {}{}): {}{} Throws {}.Error",
                       method.deprecated ? "@Deprecated " : "", fb_name(method.name),
                       local("request_"), input, method.client_streaming ? "[]" : "", output,
                       method.server_streaming ? "[]" : "", m_core);
        }

        m_out.dedent();
        m_out.line("End Interface");
    }
};

} // namespace

std::string generate_proto(const proto_file_t& file) {
    return proto_generator_t(file).run();
}

} // namespace fb
