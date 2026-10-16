#include "fb/lexer.hpp"
#include "printers.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <format>
#include <string>
#include <string_view>
#include <vector>

using fb::token_kind_t;

namespace {

struct lexed_t {
    std::vector<fb::token_t> tokens; // without the final END_OF_FILE
    std::vector<token_kind_t> kinds;
    std::vector<std::string> texts;
    std::vector<std::string> errors; // "begin-end: message"
};

/// Lexes `text` as a file named "test.fb". Error spans are byte offsets into `text`.
lexed_t lex_text(const std::string_view text) {
    const fb::source_file_t file(fb::file_id_t{}, "test.fb", std::string(text));
    fb::diagnostic_bag_t bag;

    lexed_t lexed;
    for (const fb::token_t& token : fb::lex(file, bag)) {
        if (token.kind != token_kind_t::END_OF_FILE) {
            lexed.tokens.push_back(token);
            lexed.kinds.push_back(token.kind);
            lexed.texts.emplace_back(file.slice(token.span));
        }
    }

    for (const fb::diagnostic_t& diagnostic : bag.diagnostics()) {
        lexed.errors.push_back(std::format("{}-{}: {}", diagnostic.span.begin, diagnostic.span.end,
                                           diagnostic.message));
    }

    return lexed;
}

} // namespace

TEST_CASE("end of file is the last token, with an empty span at the end", "[lexer]") {
    for (const std::string_view text : { "", "x  ", "x\n" }) {
        INFO(text);
        const fb::source_file_t file(fb::file_id_t{}, "test.fb", std::string(text));
        fb::diagnostic_bag_t bag;
        const std::vector<fb::token_t> tokens = fb::lex(file, bag);

        REQUIRE_FALSE(tokens.empty());
        CHECK(tokens.back().kind == token_kind_t::END_OF_FILE);
        CHECK(tokens.back().span.begin == text.size());
        CHECK(tokens.back().span.empty());
    }
}

TEST_CASE("identifiers and keywords", "[lexer]") {
    using enum token_kind_t;
    const lexed_t lexed = lex_text("If ifx If_ _x x1 Set");
    CHECK(lexed.kinds ==
          std::vector{ KW_IF, IDENTIFIER, IDENTIFIER, IDENTIFIER, IDENTIFIER, IDENTIFIER });
    CHECK(lexed.texts == std::vector<std::string>{ "If", "ifx", "If_", "_x", "x1", "Set" });
    CHECK(lexed.errors.empty());
}

TEST_CASE("every punctuation spelling lexes as itself", "[lexer]") {
    struct expected_t {
        std::string_view spelling;
        token_kind_t kind;
    };

    // One entry per FB_PUNCTUATION line in token_kinds.def.
    constexpr std::array all = {
#define FB_PUNCTUATION(name, spelling) expected_t{ spelling, token_kind_t::name },
#include "fb/token_kinds.def" // NOLINT(readability-trailing-comma)
    };

    for (const auto& [spelling, kind] : all) {
        INFO(spelling);
        CHECK(lex_text(spelling).kinds == std::vector{ kind });
    }
}

TEST_CASE("punctuation takes the longest match", "[lexer]") {
    using enum token_kind_t;
    const lexed_t lexed = lex_text("a<<=b?.c??d...e..f");
    CHECK(lexed.kinds == std::vector{ IDENTIFIER, LESS_LESS_EQUAL, IDENTIFIER, QUESTION_DOT,
                                      IDENTIFIER, QUESTION_QUESTION, IDENTIFIER, DOT_DOT_DOT,
                                      IDENTIFIER, DOT_DOT, IDENTIFIER });
}

TEST_CASE("'>>' is two '>' tokens that touch", "[lexer]") {
    // Nested generics like "Map<K, List<V>>" close with two '>'. The parser reads two that touch
    // as a shift.
    using enum token_kind_t;
    const lexed_t lexed = lex_text(">> >>=");
    REQUIRE(lexed.kinds == std::vector{ GREATER, GREATER, GREATER, GREATER_EQUAL });
    CHECK(lexed.tokens[0].span.end == lexed.tokens[1].span.begin);
    CHECK(lexed.tokens[1].span.end != lexed.tokens[2].span.begin);
    CHECK(lexed.tokens[2].span.end == lexed.tokens[3].span.begin);
}

TEST_CASE("spaces, tabs, and comments are skipped", "[lexer]") {
    const lexed_t lexed = lex_text("a \t/* b */ c // d");
    CHECK(lexed.texts == std::vector<std::string>{ "a", "c" });
    CHECK(lexed.errors.empty());
}

TEST_CASE("block comments nest", "[lexer]") {
    const lexed_t lexed = lex_text("a /* x /* y */ z */ b");
    CHECK(lexed.texts == std::vector<std::string>{ "a", "b" });
    CHECK(lexed.errors.empty());
}

TEST_CASE("a run of line breaks is one NEWLINE", "[lexer]") {
    using enum token_kind_t;
    const lexed_t lexed = lex_text("a\n\n  // c\r\n\rb\n");
    CHECK(lexed.kinds == std::vector{ IDENTIFIER, NEWLINE, IDENTIFIER, NEWLINE });

    // The NEWLINE's span is only the first line break of the run.
    CHECK(lexed.texts == std::vector<std::string>{ "a", "\n", "b", "\n" });
}

TEST_CASE("a CRLF is one NEWLINE token", "[lexer]") {
    CHECK(lex_text("a\r\nb").texts == std::vector<std::string>{ "a", "\r\n", "b" });
}

TEST_CASE("no NEWLINE comes before the first token", "[lexer]") {
    CHECK(lex_text("\n\n// c\na").kinds == std::vector{ token_kind_t::IDENTIFIER });
}

TEST_CASE("a block comment across lines is not a line break", "[lexer]") {
    CHECK(lex_text("a /*\n*/ b").texts == std::vector<std::string>{ "a", "b" });
}

TEST_CASE("a small function lexes as expected", "[lexer]") {
    using enum token_kind_t;
    const lexed_t lexed = lex_text("Function Add(a: Int, b: Int): Int\n"
                                   "    Return a + b\n"
                                   "End Function\n");
    CHECK(lexed.kinds == std::vector{ KW_FUNCTION, IDENTIFIER, LEFT_PAREN, IDENTIFIER, COLON,
                                      IDENTIFIER,  COMMA,      IDENTIFIER, COLON,      IDENTIFIER,
                                      RIGHT_PAREN, COLON,      IDENTIFIER, NEWLINE,    KW_RETURN,
                                      IDENTIFIER,  PLUS,       IDENTIFIER, NEWLINE,    KW_END,
                                      KW_FUNCTION, NEWLINE });
    CHECK(lexed.errors.empty());
}

TEST_CASE("an unterminated block comment is reported at its opening", "[lexer]") {
    // The inner comment closes, so the outer one is still open at the end.
    const lexed_t lexed = lex_text("a /* b /* c */");
    CHECK(lexed.texts == std::vector<std::string>{ "a" });
    CHECK(lexed.errors == std::vector<std::string>{ "2-4: unterminated block comment" });
}

TEST_CASE("unexpected characters become INVALID tokens", "[lexer]") {
    using enum token_kind_t;
    const lexed_t lexed = lex_text("a $ é b");
    CHECK(lexed.kinds == std::vector{ IDENTIFIER, INVALID, INVALID, IDENTIFIER });

    // é is two bytes in UTF-8, so its span is 4-6.
    CHECK(lexed.errors == std::vector<std::string>{ "2-3: unexpected character '$'",
                                                    "4-6: unexpected character U+00E9" });
}

TEST_CASE("invalid UTF-8 is reported, even in comments", "[lexer]") {
    using enum token_kind_t;
    const lexed_t lexed = lex_text("a \xFF b // \xFE");
    CHECK(lexed.kinds == std::vector{ IDENTIFIER, INVALID, IDENTIFIER });
    CHECK(lexed.errors == std::vector<std::string>{ "2-3: invalid UTF-8", "9-10: invalid UTF-8" });
}

TEST_CASE("directives aren't supported yet", "[lexer]") {
    // A '#' that starts a line skips the rest of it. Anywhere else, '#' is just unexpected.
    using enum token_kind_t;
    const lexed_t lexed = lex_text("#If DEBUG\nx\ny #");
    CHECK(lexed.kinds == std::vector{ IDENTIFIER, NEWLINE, IDENTIFIER, INVALID });
    CHECK(lexed.errors == std::vector<std::string>{
                              "0-3: conditional compilation isn't supported yet",
                              "14-15: unexpected character '#'",
                          });
}

TEST_CASE("integer literals", "[lexer]") {
    const lexed_t lexed = lex_text("42 1_000_000 0xFF00 0b1010 0o755 007");
    CHECK(lexed.kinds == std::vector<token_kind_t>(6, token_kind_t::INTEGER));
    CHECK(lexed.texts ==
          std::vector<std::string>{ "42", "1_000_000", "0xFF00", "0b1010", "0o755", "007" });
    CHECK(lexed.errors.empty());
}

TEST_CASE("decimal literals", "[lexer]") {
    const lexed_t lexed = lex_text("1.5 1.5e-3 1e5 2E+10 0.0");
    CHECK(lexed.kinds == std::vector<token_kind_t>(5, token_kind_t::DECIMAL));
    CHECK(lexed.texts == std::vector<std::string>{ "1.5", "1.5e-3", "1e5", "2E+10", "0.0" });
    CHECK(lexed.errors.empty());
}

TEST_CASE("a decimal point needs digits on both sides", "[lexer]") {
    using enum token_kind_t;
    CHECK(lex_text("1..3").kinds == std::vector{ INTEGER, DOT_DOT, INTEGER });
    CHECK(lex_text("5.ToString").kinds == std::vector{ INTEGER, DOT, IDENTIFIER });
    CHECK(lex_text(".5").kinds == std::vector{ DOT, INTEGER });

    // Hex, binary, and octal literals are always integers.
    CHECK(lex_text("0xFF.5").kinds == std::vector{ INTEGER, DOT, INTEGER });
}

TEST_CASE("a separator must sit between two digits", "[lexer]") {
    const lexed_t lexed = lex_text("1_ 1__0 0x_FF");
    CHECK(lexed.kinds == std::vector<token_kind_t>(3, token_kind_t::INTEGER));

    // After a prefix, only the digits are reported, so 10-13 is "_FF".
    CHECK(lexed.errors == std::vector<std::string>{
                              "0-2: '_' must be between two digits",
                              "3-7: '_' must be between two digits",
                              "10-13: '_' must be between two digits",
                          });
}

TEST_CASE("a prefix needs digits", "[lexer]") {
    // '2' isn't a binary digit, so "0b" has no digits, and the '2' is stuck to the number.
    const lexed_t lexed = lex_text("0x 0b2");
    CHECK(lexed.texts == std::vector<std::string>{ "0x", "0b" });
    CHECK(lexed.errors == std::vector<std::string>{
                              "0-2: expected digits after '0x'",
                              "3-5: expected digits after '0b'",
                              "5-6: unexpected '2' after number",
                          });
}

TEST_CASE("letters stuck to a number are reported, not lexed", "[lexer]") {
    const lexed_t lexed = lex_text("123abc 1.5f");
    CHECK(lexed.texts == std::vector<std::string>{ "123", "1.5" });
    CHECK(lexed.errors == std::vector<std::string>{
                              "3-6: unexpected 'abc' after number",
                              "10-11: unexpected 'f' after number",
                          });
}

TEST_CASE("integer literals must fit in 64 bits", "[lexer]") {
    // 2^64 - 1, then 2^64.
    const lexed_t lexed = lex_text("18446744073709551615 18446744073709551616");
    CHECK(lexed.kinds == std::vector<token_kind_t>(2, token_kind_t::INTEGER));
    CHECK(lexed.errors ==
          std::vector<std::string>{ "21-41: integer literal doesn't fit in 64 bits" });
}

TEST_CASE("character literals", "[lexer]") {
    const lexed_t lexed = lex_text("'A' 'é' '\\n' '\\u{1F600}' '{' '\\''");
    CHECK(lexed.kinds == std::vector<token_kind_t>(6, token_kind_t::CHARACTER));
    CHECK(lexed.texts ==
          std::vector<std::string>{ "'A'", "'é'", "'\\n'", "'\\u{1F600}'", "'{'", "'\\''" });
    CHECK(lexed.errors.empty());
}

TEST_CASE("a character literal holds exactly one character", "[lexer]") {
    const lexed_t lexed = lex_text("'' 'ab'");
    CHECK(lexed.kinds == std::vector<token_kind_t>(2, token_kind_t::CHARACTER));
    CHECK(lexed.errors == std::vector<std::string>{
                              "0-2: empty character literal",
                              "3-7: character literal holds more than one character",
                          });
}

TEST_CASE("an unterminated character literal stops at the line break", "[lexer]") {
    using enum token_kind_t;
    const lexed_t lexed = lex_text("'a\nb");
    CHECK(lexed.kinds == std::vector{ CHARACTER, NEWLINE, IDENTIFIER });
    CHECK(lexed.errors == std::vector<std::string>{ "0-2: unterminated character literal" });
}

TEST_CASE("bad escapes in character literals are reported", "[lexer]") {
    const lexed_t lexed = lex_text(R"('\q' '\x80' '\u{D800}')");
    CHECK(lexed.kinds == std::vector<token_kind_t>(3, token_kind_t::CHARACTER));

    // U+D800 is half of a UTF-16 surrogate pair, which UTF-8 text can't hold.
    CHECK(lexed.errors == std::vector<std::string>{
                              "1-3: unknown escape sequence",
                              "6-10: \\x is for ASCII (00-7F); use \\u{...} instead",
                              "13-21: not a Unicode scalar value",
                          });
}

TEST_CASE("plain strings", "[lexer]") {
    const lexed_t lexed = lex_text(R"("hello" "" "a\"b" "tab\tend")");
    CHECK(lexed.kinds == std::vector<token_kind_t>(4, token_kind_t::STRING));
    CHECK(lexed.texts ==
          std::vector<std::string>{ R"("hello")", R"("")", R"("a\"b")", R"("tab\tend")" });
    CHECK(lexed.errors.empty());
}

TEST_CASE("doubled braces and escapes don't interpolate", "[lexer]") {
    const lexed_t lexed = lex_text(R"("{{x}} \u{1F600}")");
    CHECK(lexed.kinds == std::vector{ token_kind_t::STRING });
    CHECK(lexed.errors.empty());
}

TEST_CASE("a lone '}' in a string is reported", "[lexer]") {
    const lexed_t lexed = lex_text(R"("a}b")");
    CHECK(lexed.kinds == std::vector{ token_kind_t::STRING });
    CHECK(lexed.errors ==
          std::vector<std::string>{ "2-3: a lone '}' in a string must be written '}}'" });
}

TEST_CASE("bad escapes in strings are reported", "[lexer]") {
    CHECK(lex_text(R"("\q")").errors == std::vector<std::string>{ "1-3: unknown escape sequence" });
}

TEST_CASE("an unterminated string stops at the line break", "[lexer]") {
    using enum token_kind_t;
    const lexed_t lexed = lex_text("\"abc\nx");
    CHECK(lexed.kinds == std::vector{ STRING, NEWLINE, IDENTIFIER });
    CHECK(lexed.texts == std::vector<std::string>{ "\"abc", "\n", "x" });
    CHECK(lexed.errors == std::vector<std::string>{ "0-1: unterminated string" });
}

TEST_CASE("interpolation splits a string around its expressions", "[lexer]") {
    using enum token_kind_t;
    const lexed_t lexed = lex_text(R"("HP: {hp}/{max}")");
    CHECK(lexed.kinds ==
          std::vector{ STRING_START, IDENTIFIER, STRING_MIDDLE, IDENTIFIER, STRING_END });
    CHECK(lexed.texts == std::vector<std::string>{ R"("HP: {)", "hp", "}/{", "max", R"(}")" });
    CHECK(lexed.errors.empty());
}

TEST_CASE("braces inside an interpolation nest", "[lexer]") {
    using enum token_kind_t;
    const lexed_t lexed = lex_text(R"("{m({"a": 1})}")");
    CHECK(lexed.kinds == std::vector{ STRING_START, IDENTIFIER, LEFT_PAREN, LEFT_BRACE, STRING,
                                      COLON, INTEGER, RIGHT_BRACE, RIGHT_PAREN, STRING_END });
    CHECK(lexed.errors.empty());
}

TEST_CASE("strings nest inside interpolations", "[lexer]") {
    using enum token_kind_t;
    CHECK(lex_text(R"("{name ?? "nobody"}")").kinds ==
          std::vector{ STRING_START, IDENTIFIER, QUESTION_QUESTION, STRING, STRING_END });

    const lexed_t lexed = lex_text(R"("a{"b{c}d"}e")");
    CHECK(lexed.kinds ==
          std::vector{ STRING_START, STRING_START, IDENTIFIER, STRING_END, STRING_END });
    CHECK(lexed.texts == std::vector<std::string>{ R"("a{)", R"("b{)", "c", R"(}d")", R"(}e")" });
    CHECK(lexed.errors.empty());
}

TEST_CASE("a single-line string can't continue past a line break inside an interpolation",
          "[lexer]") {
    using enum token_kind_t;
    const lexed_t lexed = lex_text("\"a{b\nc");
    CHECK(lexed.kinds == std::vector{ STRING_START, IDENTIFIER, NEWLINE, IDENTIFIER });
    CHECK(lexed.errors == std::vector<std::string>{ "0-1: unterminated string" });
}

TEST_CASE("a string still open at end of file is reported", "[lexer]") {
    // Each error points at the opening quotes.
    CHECK(lex_text(R"("a{b)").errors == std::vector<std::string>{ "0-1: unterminated string" });
    CHECK(lex_text(R"(r"abc)").errors == std::vector<std::string>{ "0-2: unterminated string" });
    CHECK(lex_text("\"\"\"\n  a").errors == std::vector<std::string>{ "0-3: unterminated string" });
}

TEST_CASE("raw strings have no escapes or interpolation", "[lexer]") {
    const lexed_t lexed = lex_text(R"(r"C:\raw\{x}" r"")");
    CHECK(lexed.kinds == std::vector<token_kind_t>(2, token_kind_t::STRING));
    CHECK(lexed.texts == std::vector<std::string>{ R"(r"C:\raw\{x}")", R"(r"")" });
    CHECK(lexed.errors.empty());
}

TEST_CASE("'r' not followed by a quote is an identifier", "[lexer]") {
    CHECK(lex_text("r rx r1").kinds == std::vector<token_kind_t>(3, token_kind_t::IDENTIFIER));
}

TEST_CASE("multi-line strings", "[lexer]") {
    const std::string text = "\"\"\"\n"
                             "    hello\n"
                             "      world\n"
                             "\n"
                             "    \"\"\"";
    const lexed_t lexed = lex_text(text);
    CHECK(lexed.kinds == std::vector{ token_kind_t::STRING });
    CHECK(lexed.texts == std::vector{ text });
    CHECK(lexed.errors.empty());
}

TEST_CASE("multi-line strings work with CRLF line breaks", "[lexer]") {
    const lexed_t lexed = lex_text("\"\"\"\r\n  a\r\n  \"\"\"");
    CHECK(lexed.kinds == std::vector{ token_kind_t::STRING });
    CHECK(lexed.errors.empty());
}

TEST_CASE("every line of text starts with the closing quotes' indentation", "[lexer]") {
    const lexed_t lexed = lex_text("\"\"\"\n"
                                   "    a\n"
                                   "  b\n"
                                   "\n"
                                   "    \"\"\"");
    CHECK(lexed.kinds == std::vector{ token_kind_t::STRING });

    // "b" is indented 2 spaces where the closing quotes have 4, so 10-12 is its indentation.
    // The blank line isn't checked.
    CHECK(lexed.errors == std::vector<std::string>{
                              R"(10-12: line must start with the closing """'s indentation)",
                          });
}

TEST_CASE("the closing quotes stand on their own line", "[lexer]") {
    CHECK(lex_text("\"\"\"\n  a\"\"\"").errors ==
          std::vector<std::string>{ R"(7-10: the closing """ must be on its own line)" });
}

TEST_CASE("the opening quotes end their line", "[lexer]") {
    CHECK(lex_text("\"\"\"a\n\"\"\"").errors ==
          std::vector<std::string>{
              R"(3-4: a multi-line string starts on the line after its opening """)",
          });
}

TEST_CASE("multi-line strings can interpolate", "[lexer]") {
    using enum token_kind_t;
    const lexed_t lexed = lex_text("\"\"\"\n  {a}\n  \"\"\"");
    CHECK(lexed.kinds == std::vector{ STRING_START, IDENTIFIER, STRING_END });
    CHECK(lexed.texts == std::vector<std::string>{ "\"\"\"\n  {", "a", "}\n  \"\"\"" });
    CHECK(lexed.errors.empty());
}

TEST_CASE("raw multi-line strings may contain quotes and braces", "[lexer]") {
    const lexed_t lexed = lex_text("r\"\"\"\n  say \"hi\" {x}\n  \"\"\"");
    CHECK(lexed.kinds == std::vector{ token_kind_t::STRING });
    CHECK(lexed.errors.empty());
}
