#include "fb/lexer.hpp"
#include "printers.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <format>
#include <span>
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

/// Lexes `text` as a file named "test.fb", with `defined` the symbols #If finds. Error spans are
/// byte offsets into `text`.
lexed_t lex_text(const std::string_view text,
                 const std::span<const std::string_view> defined = {}) {
    const fb::source_file_t file(fb::file_id_t{}, "test.fb", std::string(text));
    fb::diagnostic_bag_t bag;

    lexed_t lexed;
    for (const fb::token_t& token : fb::lex(file, bag, defined)) {
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

TEST_CASE("#If keeps the lines of the branch whose condition holds", "[lexer]") {
    constexpr std::array<std::string_view, 2> debug = { "DEBUG", "PLATFORM_WINDOWS" };
    constexpr std::array<std::string_view, 1> release = { "RELEASE" };
    const std::string_view text = "#If DEBUG\n"
                                  "a\n"
                                  "#ElseIf RELEASE\n"
                                  "b\n"
                                  "#Else\n"
                                  "c\n"
                                  "#End If\n"
                                  "d";
    CHECK(lex_text(text, debug).texts == std::vector<std::string>{ "a", "\n", "d" });
    CHECK(lex_text(text, release).texts == std::vector<std::string>{ "b", "\n", "d" });
    CHECK(lex_text(text).texts == std::vector<std::string>{ "c", "\n", "d" });

    // The lines of a branch that isn't taken give no tokens, and their mistakes aren't reported.
    const lexed_t skipped = lex_text("#If RELEASE\nx \"unterminated @@\n#End If\ny", debug);
    CHECK(skipped.texts == std::vector<std::string>{ "y" });
    CHECK(skipped.errors.empty());

    // Conditions combine symbols, and #Ifs nest.
    const std::string_view combined = "#If !RELEASE && (DEBUG || PLATFORM_LINUX)\na\n#End If";
    // The line break after a line that's kept still ends its statement.
    CHECK(lex_text(combined, debug).texts == std::vector<std::string>{ "a", "\n" });
    CHECK(lex_text(combined, release).texts.empty());
    CHECK(lex_text("#If DEBUG\n#If RELEASE\na\n#Else\nb\n#End If\n#End If", debug).texts ==
          std::vector<std::string>{ "b", "\n" });
    CHECK(lex_text("#If RELEASE\n#If DEBUG\na\n#End If\n#Else\nb\n#End If", debug).texts ==
          std::vector<std::string>{ "b", "\n" });
}

TEST_CASE("a line in a comment or a string is no directive, in a branch taken or not", "[lexer]") {
    constexpr std::array<std::string_view, 1> debug = { "DEBUG" };
    constexpr std::array<std::string_view, 1> release = { "RELEASE" };
    const auto both = [&](const std::string_view text, const std::vector<std::string>& in_debug,
                          const std::vector<std::string>& in_release) {
        INFO(text);
        const lexed_t debug_lexed = lex_text(text, debug);
        const lexed_t release_lexed = lex_text(text, release);
        CHECK(debug_lexed.texts == in_debug);
        CHECK(debug_lexed.errors.empty());
        CHECK(release_lexed.texts == in_release);
        CHECK(release_lexed.errors.empty());
    };

    both("#If RELEASE\na\n#Else\nb\n/*\n#End If\n*/\n#End If\nc", { "b", "\n", "c" },
         { "a", "\n", "c" });
    both("#If RELEASE\na\n#Else\ns = \"\"\"\n    #End If\n    \"\"\"\n#End If\nc",
         { "s", "=", "\"\"\"\n    #End If\n    \"\"\"", "\n", "c" }, { "a", "\n", "c" });
    // A comment in a line that isn't taken ends where it would in one that is, even in a string.
    both("#If RELEASE\na\n#Else\ns = \"/*\"\n#End If\nc", { "s", "=", "\"/*\"", "\n", "c" },
         { "a", "\n", "c" });

    // A block comment after a directive can go on past its line.
    both("#If DEBUG /* a\nb */\nx\n#End If", { "x", "\n" }, {});
    both("#If DEBUG\nx\n#Else /* a\n#End If\n*/\ny\n#End If", { "x", "\n" }, { "y", "\n" });
    both("#If RELEASE\nx\n#ElseIf DEBUG /* a\n#End If\n*/\ny\n#End If", { "y", "\n" },
         { "x", "\n" });
    CHECK(lex_text("#If DEBUG /* a\nb */ c\n#End If", debug).errors ==
          std::vector<std::string>{ "20-21: expected the end of the line" });

    // What runs past a branch that isn't taken is reported, since the directives after it are
    // in it.
    CHECK(lex_text("#If RELEASE\n/* a\n#End If", debug).errors ==
          std::vector<std::string>{ "12-14: unterminated block comment",
                                    "0-3: this #If needs an #End If" });
    CHECK(
        lex_text("#If RELEASE\ns = \"\"\"\n#End If", debug).errors ==
        std::vector<std::string>{ "16-19: unterminated string", "0-3: this #If needs an #End If" });
}

TEST_CASE("invalid UTF-8 is an error in a branch that isn't taken too", "[lexer]") {
    constexpr std::array<std::string_view, 1> debug = { "DEBUG" };
    CHECK(lex_text("#If RELEASE\nx = \"\xFF\"\n#End If\ny", debug).errors ==
          std::vector<std::string>{ "17-18: invalid UTF-8" });
    CHECK(lex_text("#If RELEASE\n\xFE\n#End If", debug).errors ==
          std::vector<std::string>{ "12-13: invalid UTF-8" });
    CHECK(lex_text("#If DEBUG\n#Else\n#If RELEASE \xFE\n#End If\n#End If", debug).errors ==
          std::vector<std::string>{ "28-29: invalid UTF-8" });
}

TEST_CASE("#If mistakes", "[lexer]") {
    using enum token_kind_t;
    CHECK(lex_text("#Else").errors ==
          std::vector<std::string>{ "0-5: #Else needs an #If before it" });
    CHECK(lex_text("#End If").errors ==
          std::vector<std::string>{ "0-7: #End If needs an #If before it" });
    CHECK(lex_text("#If DEBUG\nx").errors ==
          std::vector<std::string>{ "0-3: this #If needs an #End If" });
    CHECK(lex_text("#If DEBUG\n#Else\n#Else\n#End If").errors ==
          std::vector<std::string>{ "16-21: #Else can't come after #Else" });
    CHECK(lex_text("#Pragma x").errors ==
          std::vector<std::string>{
              "0-7: '#Pragma' isn't a directive; there are #If, #ElseIf, #Else, and #End If" });
    CHECK(lex_text("#If\n#End If").errors ==
          std::vector<std::string>{ "3-4: expected a symbol, like DEBUG" });
    CHECK(lex_text("#If (DEBUG\n#End If").errors ==
          std::vector<std::string>{ "4-5: this '(' needs a ')'" });
    CHECK(lex_text("#If DEBUG DEBUG\n#End If").errors ==
          std::vector<std::string>{ "10-11: expected the end of the line" });
    CHECK(lex_text("#If DEBG\n#End If").errors ==
          std::vector<std::string>{ "4-8: 'DEBG' is never defined; #If can test DEBUG, RELEASE, "
                                    "PLATFORM_WINDOWS, PLATFORM_LINUX, and PLATFORM_MACOS" });

    // A condition cut off by the end of the file gets an empty span there, and no span goes past
    // the end.
    CHECK(lex_text("#If !").errors ==
          std::vector<std::string>{ "5-5: expected a symbol, like DEBUG",
                                    "0-3: this #If needs an #End If" });
    for (const std::string_view text : { "#If !", "#If (", "#If DEBUG ||", "#If (!" }) {
        INFO(text);
        const fb::source_file_t file(fb::file_id_t{}, "test.fb", std::string(text));
        fb::diagnostic_bag_t bag;
        const std::vector<fb::token_t> tokens = fb::lex(file, bag);
        CHECK(tokens.back().span.end <= text.size());
        for (const fb::diagnostic_t& diagnostic : bag.diagnostics()) {
            CHECK(diagnostic.span.end <= text.size());
        }
    }

    // A condition nests up to 1000 deep, as code does, so reading it can't run out of stack.
    const auto parenthesized = [](const std::size_t depth) {
        return std::format("#If {}DEBUG{}\n#End If", std::string(depth, '('),
                           std::string(depth, ')'));
    };
    const std::string too_deep =
        ": this is nested more than 1000 deep, deeper than the compiler goes";
    CHECK(lex_text(parenthesized(1000)).errors.empty());
    CHECK(lex_text(parenthesized(1001)).errors ==
          std::vector<std::string>{ "1004-1005" + too_deep });
    CHECK(lex_text(parenthesized(300000)).errors ==
          std::vector<std::string>{ "1004-1005" + too_deep });
    CHECK(lex_text("#If " + std::string(1000, '!') + "DEBUG\n#End If").errors.empty());
    CHECK(lex_text("#If " + std::string(300000, '!') + "DEBUG\n#End If").errors ==
          std::vector<std::string>{ "1004-1005" + too_deep });

    // Anywhere but the start of a line, '#' is unexpected.
    const lexed_t lexed = lex_text("y #");
    CHECK(lexed.kinds == std::vector{ IDENTIFIER, INVALID });
    CHECK(lexed.errors == std::vector<std::string>{ "2-3: unexpected character '#'" });
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
    // A Duration's number and unit, as std.time writes one.
    CHECK(lex_text("2.Seconds").kinds == std::vector{ INTEGER, DOT, IDENTIFIER });
    CHECK(lex_text("1.5.Minutes").kinds == std::vector{ DECIMAL, DOT, IDENTIFIER });
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

TEST_CASE("a ':' in an interpolation starts its format spec, outside brackets, with no space after "
          "it",
          "[lexer]") {
    using enum token_kind_t;
    const lexed_t lexed = lex_text(R"("{price:.2} and {x:>8,}")");
    CHECK(lexed.kinds == std::vector{ STRING_START, IDENTIFIER, FORMAT_SPEC, STRING_MIDDLE,
                                      IDENTIFIER, FORMAT_SPEC, STRING_END });
    CHECK(lexed.texts ==
          std::vector<std::string>{ R"("{)", "price", ":.2", "} and {", "x", ":>8,", R"(}")" });
    CHECK(lexed.errors.empty());

    // A ternary's ':' and a function type's have a space after them, and a ':' after the
    // ternary starts the spec.
    CHECK(lex_text(R"("{a ? b : c}")").kinds == std::vector{ STRING_START, IDENTIFIER, QUESTION,
                                                             IDENTIFIER, COLON, IDENTIFIER,
                                                             STRING_END });
    CHECK(lex_text(R"("{a ? b : c:>4}")").kinds == std::vector{ STRING_START, IDENTIFIER, QUESTION,
                                                                IDENTIFIER, COLON, IDENTIFIER,
                                                                FORMAT_SPEC, STRING_END });
    CHECK(lex_text(R"("{f Is Func(Int): Int}")").kinds ==
          std::vector{ STRING_START, IDENTIFIER, KW_IS, KW_FUNC, LEFT_PAREN, IDENTIFIER,
                       RIGHT_PAREN, COLON, IDENTIFIER, STRING_END });
    CHECK(lex_text(R"("{a ? b :c}")").kinds ==
          std::vector{ STRING_START, IDENTIFIER, QUESTION, IDENTIFIER, FORMAT_SPEC, STRING_END });

    // One in brackets is the expression's.
    CHECK(lex_text(R"("{f(x:1):?}")").kinds == std::vector{ STRING_START, IDENTIFIER, LEFT_PAREN,
                                                            IDENTIFIER, COLON, INTEGER, RIGHT_PAREN,
                                                            FORMAT_SPEC, STRING_END });
    CHECK(lex_text(R"("{x As Int?:?}")").kinds == std::vector{ STRING_START, IDENTIFIER, KW_AS,
                                                               IDENTIFIER, QUESTION, FORMAT_SPEC,
                                                               STRING_END });

    // A spec is raw text, without braces.
    CHECK(lex_text(R"("{x:'*>#,e}")").errors.empty());
    CHECK(lex_text(R"("{x:a{b}")").errors ==
          std::vector<std::string>{ "5-6: a format spec can't hold a brace" });
}

TEST_CASE("a single-line string can't continue past a line break inside an interpolation",
          "[lexer]") {
    using enum token_kind_t;
    // The INVALID token tells the parser the lexer reported it, so the parser says no more.
    const lexed_t lexed = lex_text("\"a{b\nc");
    CHECK(lexed.kinds == std::vector{ STRING_START, IDENTIFIER, INVALID, NEWLINE, IDENTIFIER });
    CHECK(lexed.errors ==
          std::vector<std::string>{ "0-1: unterminated string: a '{' in it isn't closed on its "
                                    "line, and a string ends with its line" });

    // Nor through a multi-line string in its interpolation, which spans lines.
    CHECK(lex_text("\"a{\"\"\"\n    x\n    \"\"\"}b\"").errors ==
          std::vector<std::string>{ "3-6: a multi-line string can't go in an interpolation of a "
                                    "single-line string, which ends with its line" });
    CHECK(lex_text("\"\"\"\n  {\"a{r\"\"\"\n    x\n    \"\"\"}\"}\n  \"\"\"").errors ==
          std::vector<std::string>{ "10-14: a multi-line string can't go in an interpolation of a "
                                    "single-line string, which ends with its line" });

    // In a multi-line string's interpolation, one is fine.
    CHECK(lex_text("\"\"\"\n  {\"\"\"\n    x\n    \"\"\"}\n  \"\"\"").errors.empty());
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

    // An interpolation can span lines, and a line that starts inside one isn't string text, so
    // its indentation isn't checked.
    const lexed_t spanning = lex_text("\"\"\"\n    one {x +\n1} two\n    three\n    \"\"\"");
    CHECK(spanning.kinds ==
          std::vector{ STRING_START, IDENTIFIER, PLUS, NEWLINE, INTEGER, STRING_END });
    CHECK(spanning.errors.empty());
}

TEST_CASE("raw multi-line strings may contain quotes and braces", "[lexer]") {
    const lexed_t lexed = lex_text("r\"\"\"\n  say \"hi\" {x}\n  \"\"\"");
    CHECK(lexed.kinds == std::vector{ token_kind_t::STRING });
    CHECK(lexed.errors.empty());
}
