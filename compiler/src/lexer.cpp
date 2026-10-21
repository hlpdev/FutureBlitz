#include "fb/lexer.hpp"

#include "fb/literal.hpp"
#include "fb/utf8.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace fb {
namespace {

/// A string literal that's still open. While one of its interpolations is lexed, it waits on
/// the lexer's stack, and the "}" that ends the interpolation picks it back up.
struct string_state_t {
    /// [begin, open_end) is the opening quotes with any "r". "unterminated string" points there.
    std::uint32_t begin = 0;
    std::uint32_t open_end = 0;
    /// r"...": no escapes and no interpolation.
    bool raw = false;
    /// Opened with `"""`.
    bool multi_line = false;
    /// "{" tokens opened and not yet closed inside the current interpolation. A "}" at 0 ends
    /// the interpolation instead.
    std::uint32_t brace_depth = 0;
    /// Multi-line strings only: where each line after the opening quotes starts. Once the
    /// closing quotes are found, the last one is their line.
    std::vector<std::uint32_t> line_starts;
};

struct punctuation_t {
    std::string_view spelling;
    token_kind_t kind;
};

/// The symbols an #If can test. Each is true when the build defines it.
constexpr auto condition_symbols = std::to_array<std::string_view>(
    { "DEBUG", "RELEASE", "PLATFORM_WINDOWS", "PLATFORM_LINUX", "PLATFORM_MACOS" });

/// An #If whose #End If hasn't come yet.
struct condition_t {
    /// The "#If", where a missing #End If is reported.
    span_t span;
    /// The lines of the branch being read are lexed.
    bool active = false;
    /// A branch has been active already, so the rest aren't.
    bool taken = false;
    /// #Else came, so nothing but #End If can.
    bool otherwise = false;
    /// The #If is in lines that are lexed. In ones that aren't, no branch is.
    bool outer = false;
};

constexpr std::array punctuation = {
#define FB_PUNCTUATION(name, spelling) punctuation_t{ spelling, token_kind_t::name },
#include "fb/token_kinds.def" // NOLINT(readability-trailing-comma)
};

/// ASCII only. Unicode identifiers may come in a later version.
constexpr bool is_identifier_start(const char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

constexpr bool is_identifier_continue(const char c) {
    return is_identifier_start(c) || (c >= '0' && c <= '9');
}

constexpr bool is_digit(const char c, const std::uint32_t base) {
    return digit_value(c) < base;
}

/// One pass over a file. Errors go to the bag and lexing carries on, so the parser always gets
/// every token, ending with END_OF_FILE.
class lexer_t {
public:
    lexer_t(const source_file_t& file, diagnostic_bag_t& diagnostics,
            const std::span<const std::string_view> defined)
        : m_file(file), m_text(file.text()), m_diagnostics(diagnostics), m_defined(defined) {}

    std::vector<token_t> run() {
        while (true) {
            skip_trivia();
            if (at_end()) {
                break;
            }

            lex_token();
        }

        // Strings whose interpolation was still open at the end of the file.
        for (const string_state_t& state : m_strings) {
            report_unterminated(state);
        }

        for (const condition_t& condition : m_conditions) {
            m_diagnostics.error(condition.span, "this #If needs an #End If");
        }

        add(token_kind_t::END_OF_FILE, m_pos);
        return std::move(m_tokens);
    }

private:
    const source_file_t& m_file;
    std::string_view m_text;
    diagnostic_bag_t& m_diagnostics;
    std::span<const std::string_view> m_defined;
    std::vector<token_t> m_tokens;
    /// Strings waiting while one of their interpolations is lexed, innermost last.
    std::vector<string_state_t> m_strings;
    /// The #Ifs around the current line, innermost last.
    std::vector<condition_t> m_conditions;
    std::uint32_t m_pos = 0;

    [[nodiscard]] bool at_end() const {
        return m_pos >= m_text.size();
    }

    /// '\0' past the end, which matches nothing the lexer looks for.
    [[nodiscard]] char peek(const std::uint32_t ahead = 0) const {
        const std::size_t pos = m_pos + ahead;
        return pos < m_text.size() ? m_text[pos] : '\0';
    }

    [[nodiscard]] span_t span(const std::uint32_t begin, const std::uint32_t end) const {
        return { .file = m_file.id(), .begin = begin, .end = end };
    }

    void add(const token_kind_t kind, const std::uint32_t begin) {
        m_tokens.push_back({ .kind = kind, .span = span(begin, m_pos) });
    }

    /// True when no token has been added since the last line break.
    [[nodiscard]] bool at_line_start() const {
        return m_tokens.empty() || m_tokens.back().kind == token_kind_t::NEWLINE;
    }

    /// Spaces, tabs, and comments. Line breaks aren't trivia: they become NEWLINE tokens.
    void skip_trivia() {
        while (!at_end()) {
            const char c = peek();
            if (c == ' ' || c == '\t') {
                ++m_pos;
            } else if (c == '/' && peek(1) == '/') {
                skip_to_line_end();
            } else if (c == '/' && peek(1) == '*') {
                skip_block_comment();
            } else {
                return;
            }
        }
    }

    /// Moves past one character of a comment or skipped line, reporting invalid UTF-8.
    void skip_character() {
        const decoded_t decoded = decode_utf8(m_text, m_pos);
        if (!decoded.valid()) {
            m_diagnostics.error(span(m_pos, m_pos + 1), "invalid UTF-8");
        }

        m_pos += decoded.length;
    }

    /// Stops before the line break, so it still becomes a NEWLINE.
    void skip_to_line_end() {
        while (!at_end() && peek() != '\n' && peek() != '\r') {
            skip_character();
        }
    }

    /// Block comments nest. Line breaks inside are skipped too, so a comment that spans lines
    /// never ends a statement. An unterminated one is reported at its opening "/*".
    void skip_block_comment() {
        const std::uint32_t begin = m_pos;
        m_pos += 2;

        std::uint32_t depth = 1;
        while (depth > 0) {
            if (at_end()) {
                m_diagnostics.error(span(begin, begin + 2), "unterminated block comment");
                return;
            }

            if (peek() == '/' && peek(1) == '*') {
                m_pos += 2;
                ++depth;
            } else if (peek() == '*' && peek(1) == '/') {
                m_pos += 2;
                --depth;
            } else {
                skip_character();
            }
        }
    }

    void lex_token() {
        const char c = peek();
        if (c == '\n' || c == '\r') {
            lex_newline();
        } else if (c == '"' || (c == 'r' && peek(1) == '"')) {
            lex_string();
        } else if (is_identifier_start(c)) {
            lex_identifier();
        } else if (is_digit(c, 10)) {
            lex_number();
        } else if (c == '\'') {
            lex_character();
        } else if (c == '#' && at_line_start()) {
            // Directives are the first thing on their line. Anywhere else, "#" is unexpected.
            lex_directive();
        } else if (c == '}' && !m_strings.empty() && m_strings.back().brace_depth == 0) {
            // A "}" that closes no "{" of its own ends an interpolation, and its string goes on.
            continue_string();
        } else if (lex_punctuation()) {
            track_brace_depth();
        } else {
            lex_unexpected();
        }
    }

    void skip_line_break() {
        m_pos += (peek() == '\r' && peek(1) == '\n') ? 2U : 1U;
    }

    /// Whether a line break ends the statement is the parser's call. It skips NEWLINE tokens
    /// inside brackets and after an operator, "=", or ",".
    void lex_newline() {
        // A single-line string can't span lines, even inside an interpolation, so any still
        // waiting end here. A multi-line one may, so it and the strings around it stay open.
        // The parser takes the INVALID token for the error, so what it would say about the rest
        // of the expression isn't said too.
        while (!m_strings.empty() && !m_strings.back().multi_line) {
            const string_state_t& state = m_strings.back();
            m_diagnostics.error(span(state.begin, state.open_end),
                                "unterminated string: a '{{' in it isn't closed on its line, and "
                                "a string ends with its line");
            m_strings.pop_back();
            add(token_kind_t::INVALID, m_pos);
        }

        const std::uint32_t begin = m_pos;
        skip_line_break();
        // Blank and comment-only lines add nothing, and no NEWLINE comes before the first token.
        if (!at_line_start()) {
            add(token_kind_t::NEWLINE, begin);
        }
    }

    void lex_identifier() {
        const std::uint32_t begin = m_pos;
        while (is_identifier_continue(peek())) {
            ++m_pos;
        }

        const std::string_view text = m_text.substr(begin, m_pos - begin);
        add(keyword_kind(text).value_or(token_kind_t::IDENTIFIER), begin);
    }

    /// 16, 2, or 8 for a "0x", "0b", or "0o" prefix, else 10. Prefixes are lowercase only.
    [[nodiscard]] std::uint32_t prefix_base() const {
        if (peek() != '0') {
            return 10;
        }

        switch (peek(1)) {
        case 'x':
            return 16;
        case 'b':
            return 2;
        case 'o':
            return 8;
        default:
            return 10;
        }
    }

    void lex_number() {
        const std::uint32_t begin = m_pos;
        token_kind_t kind = token_kind_t::INTEGER;
        bool valid = true;

        // Prefixed numbers are always integers: "0xFF.5" is "0xFF", ".", "5".
        if (const std::uint32_t base = prefix_base(); base != 10) {
            m_pos += 2;
            const std::uint32_t digits = m_pos;
            valid = scan_digits(base);
            if (m_pos == digits) {
                m_diagnostics.error(span(begin, m_pos), "expected digits after '{}'",
                                    m_text.substr(begin, 2));
            }
        } else {
            valid = scan_digits(10);
            // A "." starts a fraction only when a digit follows, so "1..3" and "5.ToString"
            // begin with an INTEGER. An exponent also needs a digit after its sign.
            if (peek() == '.' && is_digit(peek(1), 10)) {
                ++m_pos;
                valid = scan_digits(10) && valid;
                kind = token_kind_t::DECIMAL;
            }

            const std::uint32_t sign = (peek(1) == '+' || peek(1) == '-') ? 1U : 0U;
            if ((peek() == 'e' || peek() == 'E') && is_digit(peek(1 + sign), 10)) {
                m_pos += 1 + sign;
                valid = scan_digits(10) && valid;
                kind = token_kind_t::DECIMAL;
            }
        }

        const std::string_view text = m_text.substr(begin, m_pos - begin);
        add(kind, begin);
        // Malformed digits were already reported. Some, like a bare "0x", would fail here too.
        if (valid && kind == token_kind_t::INTEGER && !parse_integer(text).has_value()) {
            m_diagnostics.error(span(begin, m_pos), "integer literal doesn't fit in 64 bits");
        }

        skip_number_suffix();
    }

    /// Digits of `base`, with "_" separators. False when there are none (the caller reports
    /// that) or when a "_" isn't between two digits (reported here).
    bool scan_digits(const std::uint32_t base) {
        const std::uint32_t begin = m_pos;
        while (is_digit(peek(), base) || peek() == '_') {
            ++m_pos;
        }

        const std::string_view digits = m_text.substr(begin, m_pos - begin);
        if (digits.empty()) {
            return false;
        }

        if (digits.starts_with('_') || digits.ends_with('_') || digits.contains("__")) {
            m_diagnostics.error(span(begin, m_pos), "'_' must be between two digits");
            return false;
        }

        return true;
    }

    /// Letters or digits stuck to a number, like the "f" in "1.5f" or the "2" in "0b2", are
    /// reported and dropped rather than lexed as an identifier.
    void skip_number_suffix() {
        const std::uint32_t begin = m_pos;
        while (is_identifier_continue(peek())) {
            ++m_pos;
        }

        if (m_pos > begin) {
            m_diagnostics.error(span(begin, m_pos), "unexpected '{}' after number",
                                m_text.substr(begin, m_pos - begin));
        }
    }

    /// A character literal holds exactly one character or escape. An unterminated one stops at
    /// the line break.
    void lex_character() {
        const std::uint32_t begin = m_pos;
        ++m_pos;

        std::uint32_t count = 0;
        bool valid = true;

        while (!at_end() && peek() != '\'' && peek() != '\n' && peek() != '\r') {
            if (peek() == '\\') {
                valid = skip_escape() && valid;
            } else {
                skip_character();
            }

            ++count;
        }

        if (peek() != '\'') {
            add(token_kind_t::CHARACTER, begin);
            m_diagnostics.error(span(begin, m_pos), "unterminated character literal");
            return;
        }

        ++m_pos;
        add(token_kind_t::CHARACTER, begin);
        // After a bad escape, which was already reported, the count isn't worth a second error.
        if (valid && count == 0) {
            m_diagnostics.error(span(begin, m_pos), "empty character literal");
        } else if (valid && count > 1) {
            m_diagnostics.error(span(begin, m_pos),
                                "character literal holds more than one character");
        }
    }

    /// Moves past the escape at the backslash, reporting it if it's bad. False when it's bad.
    bool skip_escape() {
        const escape_t escape = decode_escape(m_text, m_pos);
        if (!escape.error.empty()) {
            m_diagnostics.error(span(m_pos, m_pos + escape.length), "{}", escape.error);
        }

        m_pos += escape.length;
        return escape.error.empty();
    }

    /// Starts at the opening quotes: `"`, `"""`, or either with an "r" in front.
    void lex_string() {
        const std::uint32_t begin = m_pos;
        string_state_t state{ .begin = begin, .raw = peek() == 'r' };
        if (state.raw) {
            ++m_pos;
        }

        state.multi_line = m_text.substr(m_pos).starts_with(R"(""")");
        m_pos += state.multi_line ? 3U : 1U;
        state.open_end = m_pos;
        if (state.multi_line) {
            start_multi_line(state);
        }

        scan_string(std::move(state), begin, true);
    }

    /// The text starts on the line after the opening quotes, and only spaces and tabs may
    /// follow them.
    void start_multi_line(string_state_t& state) {
        while (peek() == ' ' || peek() == '\t') {
            ++m_pos;
        }

        if (peek() == '\n' || peek() == '\r') {
            skip_line_break();
            state.line_starts.push_back(m_pos);
        } else if (!at_end()) {
            const std::size_t line_end =
                std::min(m_text.find_first_of("\r\n", m_pos), m_text.size());
            m_diagnostics.error(span(m_pos, static_cast<std::uint32_t>(line_end)),
                                R"(a multi-line string starts on the line after its opening """)");
        }
    }

    /// Picks a waiting string back up at the "}" that ends its interpolation.
    void continue_string() {
        string_state_t state = std::move(m_strings.back());
        m_strings.pop_back();

        const std::uint32_t begin = m_pos;
        ++m_pos;
        scan_string(std::move(state), begin, false);
    }

    /// Scans one piece of a string, from `begin` (the opening quotes, or the "}" that ended an
    /// interpolation) up to the closing quotes or the next "{". `first` is true for the piece
    /// with the opening quotes.
    void scan_string(string_state_t state, const std::uint32_t begin, const bool first) {
        // The kind this piece gets if it reaches the end of the string.
        const token_kind_t whole = first ? token_kind_t::STRING : token_kind_t::STRING_END;
        const std::string_view closing = state.multi_line ? R"(""")" : R"(")";

        while (!at_end()) {
            const char c = peek();
            const bool special = !state.raw && (c == '\\' || c == '{' || c == '}');
            if (c == '\n' || c == '\r') {
                if (!state.multi_line) {
                    break;
                }

                skip_line_break();
                state.line_starts.push_back(m_pos);
            } else if (m_text.substr(m_pos).starts_with(closing)) {
                if (state.multi_line) {
                    check_indentation(state);
                }

                m_pos += static_cast<std::uint32_t>(closing.size());
                add(whole, begin);
                return;
            } else if (!special) {
                skip_character();
            } else if (c == '\\') {
                skip_escape();
            } else if (peek(1) == c) { // "{{" or "}}"
                m_pos += 2;
            } else if (c == '{') {
                // An interpolation: the piece ends after the "{", and the string waits on
                // m_strings until the "}" that ends it.
                ++m_pos;
                add(first ? token_kind_t::STRING_START : token_kind_t::STRING_MIDDLE, begin);
                m_strings.push_back(std::move(state));
                return;
            } else {
                m_diagnostics.error(span(m_pos, m_pos + 1), "{}",
                                    "a lone '}' in a string must be written '}}'");
                ++m_pos;
            }
        }

        // The end of the file, or a line break in a single-line string.
        add(whole, begin);
        report_unterminated(state);
    }

    /// The closing quotes must stand on their own line, and decoding strips their indentation
    /// from every line, so each line with text must start with it. Blank lines may be shorter.
    void check_indentation(const string_state_t& state) {
        const std::uint32_t closing_line =
            state.line_starts.empty() ? state.open_end : state.line_starts.back();
        const std::string_view indent = m_text.substr(closing_line, m_pos - closing_line);
        if (state.line_starts.empty() ||
            indent.find_first_not_of(" \t") != std::string_view::npos) {
            m_diagnostics.error(span(m_pos, m_pos + 3),
                                R"(the closing """ must be on its own line)");
            return;
        }

        // Every line but the closing one. Lines that start inside an interpolation aren't
        // recorded, so they aren't checked.
        const auto text_lines = std::span(state.line_starts).first(state.line_starts.size() - 1);
        for (const std::uint32_t line_begin : text_lines) {
            const std::size_t line_end = m_text.find_first_of("\r\n", line_begin);
            const std::string_view line = m_text.substr(line_begin, line_end - line_begin);
            const std::size_t text_begin = line.find_first_not_of(" \t");
            if (text_begin != std::string_view::npos && !line.starts_with(indent)) {
                m_diagnostics.error(
                    span(line_begin, line_begin + static_cast<std::uint32_t>(text_begin)),
                    R"(line must start with the closing """'s indentation)");
            }
        }
    }

    void report_unterminated(const string_state_t& state) const {
        m_diagnostics.error(span(state.begin, state.open_end), "unterminated string");
    }

    /// Counts the braces an interpolation opens and closes itself, so only the "}" that ends it
    /// picks the string back up.
    void track_brace_depth() {
        if (m_strings.empty()) {
            return;
        }

        if (m_tokens.back().kind == token_kind_t::LEFT_BRACE) {
            ++m_strings.back().brace_depth;
        } else if (m_tokens.back().kind == token_kind_t::RIGHT_BRACE) {
            --m_strings.back().brace_depth;
        }
    }

    /// Takes the longest match, so "<<=" wins over "<<" and "<".
    bool lex_punctuation() {
        const std::string_view rest = m_text.substr(m_pos);
        const punctuation_t* best = nullptr;
        for (const punctuation_t& candidate : punctuation) {
            if (rest.starts_with(candidate.spelling) &&
                (best == nullptr || candidate.spelling.size() > best->spelling.size())) {
                best = &candidate;
            }
        }

        if (best == nullptr) {
            return false;
        }

        const std::uint32_t begin = m_pos;
        m_pos += static_cast<std::uint32_t>(best->spelling.size());
        add(best->kind, begin);
        return true;
    }

    /// "#If", "#ElseIf", "#Else", or "#End If", which give no tokens. After one, the lines of a
    /// branch that isn't taken are skipped up to the next directive, without being lexed.
    void lex_directive() {
        const std::uint32_t begin = m_pos;
        ++m_pos;
        const std::string_view name = take_word();
        const span_t where = span(begin, m_pos);
        const bool in_active = m_conditions.empty() || m_conditions.back().active;
        if (name == "If") {
            const bool outer = in_active;
            const bool holds = outer && condition_holds();
            m_conditions.push_back(
                { .span = where, .active = holds, .taken = holds, .outer = outer });
        } else if (name == "ElseIf" || name == "Else") {
            if (m_conditions.empty()) {
                m_diagnostics.error(where, "#{} needs an #If before it", name);
                skip_to_line_end();
            } else if (m_conditions.back().otherwise) {
                m_diagnostics.error(where, "#{} can't come after #Else", name);
                skip_to_line_end();
            } else {
                condition_t& condition = m_conditions.back();
                bool holds = name == "Else";
                if (!holds && condition.outer && !condition.taken) {
                    holds = condition_holds();
                } else {
                    finish_directive(holds);
                }

                condition.active = condition.outer && !condition.taken && holds;
                condition.taken = condition.taken || condition.active;
                condition.otherwise = name == "Else";
            }
        } else if (name == "End" && skip_spaces() && take_word() == "If") {
            if (m_conditions.empty()) {
                m_diagnostics.error(span(begin, m_pos), "#End If needs an #If before it");
            } else {
                m_conditions.pop_back();
            }

            finish_directive(true);
        } else {
            // In lines that aren't lexed, it's only skipped.
            if (in_active) {
                m_diagnostics.error(where,
                                    "'#{}' isn't a directive; there are #If, #ElseIf, #Else, and "
                                    "#End If",
                                    name);
            }

            skip_to_line_end();
        }

        if (!m_conditions.empty() && !m_conditions.back().active) {
            skip_inactive_lines();
        }
    }

    /// The letters after a directive's "#", or after "#End".
    std::string_view take_word() {
        const std::uint32_t begin = m_pos;
        while (is_identifier_continue(peek())) {
            ++m_pos;
        }

        return m_text.substr(begin, m_pos - begin);
    }

    /// Spaces and tabs, and true, so it can go in a condition.
    bool skip_spaces() {
        while (peek() == ' ' || peek() == '\t') {
            ++m_pos;
        }

        return true;
    }

    /// The rest of a directive's line holds nothing but a comment. `report` is false where the
    /// line is skipped anyway.
    void finish_directive(const bool report) {
        skip_spaces();
        const bool comment = peek() == '/' && (peek(1) == '/' || peek(1) == '*');
        if (report && !comment && !at_end() && peek() != '\n' && peek() != '\r') {
            m_diagnostics.error(span(m_pos, m_pos + 1), "expected the end of the line");
        }

        skip_to_line_end();
    }

    /// The lines of a branch that isn't taken, up to the next line that starts with "#". They
    /// don't need to lex.
    void skip_inactive_lines() {
        skip_to_line_end();
        while (!at_end()) {
            skip_line_break();
            skip_spaces();
            if (peek() == '#') {
                return;
            }

            while (!at_end() && peek() != '\n' && peek() != '\r') {
                ++m_pos;
            }
        }
    }

    /// An #If's or #ElseIf's condition, then the end of its line. A mistake makes it false.
    bool condition_holds() {
        const std::optional<bool> value = parse_any();
        finish_directive(value.has_value());
        return value.value_or(false);
    }

    /// "a || b": either holds.
    std::optional<bool> parse_any() {
        std::optional<bool> value = parse_all();
        while (value && skip_spaces() && peek() == '|' && peek(1) == '|') {
            m_pos += 2;
            const std::optional<bool> right = parse_all();
            value = right ? std::optional(*value || *right) : std::nullopt;
        }

        return value;
    }

    /// "a && b": both hold.
    std::optional<bool> parse_all() {
        std::optional<bool> value = parse_one();
        while (value && skip_spaces() && peek() == '&' && peek(1) == '&') {
            m_pos += 2;
            const std::optional<bool> right = parse_one();
            value = right ? std::optional(*value && *right) : std::nullopt;
        }

        return value;
    }

    /// A symbol, "!" before one, or a condition in parentheses. A symbol holds when it's defined.
    std::optional<bool> parse_one() {
        skip_spaces();
        if (peek() == '!') {
            ++m_pos;
            const std::optional<bool> value = parse_one();
            return value ? std::optional(!*value) : std::nullopt;
        }

        if (peek() == '(') {
            const std::uint32_t open = m_pos++;
            const std::optional<bool> value = parse_any();
            if (value && skip_spaces() && peek() != ')') {
                m_diagnostics.error(span(open, open + 1), "this '(' needs a ')'");
                return std::nullopt;
            }

            ++m_pos;
            return value;
        }

        const std::uint32_t begin = m_pos;
        if (!is_identifier_start(peek())) {
            m_diagnostics.error(span(m_pos, m_pos + 1), "expected a symbol, like DEBUG");
            return std::nullopt;
        }

        const std::string_view symbol = take_word();
        if (!std::ranges::contains(condition_symbols, symbol)) {
            m_diagnostics.warning(span(begin, m_pos),
                                  "'{}' is never defined; #If can test DEBUG, RELEASE, "
                                  "PLATFORM_WINDOWS, PLATFORM_LINUX, and PLATFORM_MACOS",
                                  symbol);
        }

        return std::ranges::contains(m_defined, symbol);
    }

    /// One INVALID token for a character nothing else takes, so the parser can carry on. A bad
    /// UTF-8 byte is one token on its own.
    void lex_unexpected() {
        const std::uint32_t begin = m_pos;
        const decoded_t decoded = decode_utf8(m_text, m_pos);
        m_pos += decoded.length;
        add(token_kind_t::INVALID, begin);

        // A valid character is shown as itself only when it's printable ASCII. Anything else
        // could be invisible or look like ASCII, so it's shown as U+XXXX.
        if (!decoded.valid()) {
            m_diagnostics.error(span(begin, m_pos), "invalid UTF-8");
        } else if (decoded.code_point > 0x20 && decoded.code_point < 0x7F) {
            m_diagnostics.error(span(begin, m_pos), "unexpected character '{}'",
                                static_cast<char>(decoded.code_point));
        } else {
            m_diagnostics.error(span(begin, m_pos), "unexpected character U+{:04X}",
                                static_cast<std::uint32_t>(decoded.code_point));
        }
    }
};

} // namespace

std::vector<token_t> lex(const source_file_t& file, diagnostic_bag_t& diagnostics,
                         const std::span<const std::string_view> defined) {
    return lexer_t(file, diagnostics, defined).run();
}

} // namespace fb
