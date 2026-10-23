#include "fb/token.hpp"
#include "printers.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <string_view>

using fb::describe;
using fb::keyword_kind;
using fb::token_kind_t;

TEST_CASE("every reserved keyword from the spec is a keyword", "[token]") {
    // The reserved list in docs/spec/01-lexical.md.
    constexpr auto reserved = std::to_array<std::string_view>({
        "Abstract", "Alias",    "As",      "Async",      "Await",       "Break",    "ByRef",
        "Callback", "Case",     "Catch",   "Class",      "Const",       "Continue", "Default",
        "Defer",    "Else",     "ElseIf",  "End",        "Enum",        "Extend",   "Extends",
        "Extern",   "False",    "Field",   "Final",      "Finally",     "For",      "Func",
        "Function", "Global",   "If",      "Implements", "Import",      "In",       "Interface",
        "Is",       "Local",    "Lock",    "Loop",       "Method",      "New",      "Null",
        "Operator", "Override", "Package", "Parallel",   "Private",     "Property", "Protected",
        "Public",   "ReadOnly", "Repeat",  "Return",     "Select",      "Self",     "Shared",
        "Static",   "Struct",   "Super",   "Then",       "ThreadLocal", "Throw",    "Throws",
        "To",       "True",     "Try",     "Unsafe",     "Until",       "Where",    "While",
    });

    for (const std::string_view spelling : reserved) {
        INFO(spelling);

        // describe() spells a keyword kind as written, so this also checks it's the right kind.
        CHECK(keyword_kind(spelling).transform(describe) == "'" + std::string(spelling) + "'");
    }
}

TEST_CASE("keyword_kind finds the right kind", "[token]") {
    CHECK(keyword_kind("If") == token_kind_t::KW_IF);
    CHECK(keyword_kind("ElseIf") == token_kind_t::KW_ELSEIF);
    CHECK(keyword_kind("Func") == token_kind_t::KW_FUNC);
    CHECK(keyword_kind("While") == token_kind_t::KW_WHILE);
}

TEST_CASE("miscased, contextual, and partial words aren't keywords", "[token]") {
    // Get through Weak are contextual: keywords only in one position, so they lex as identifiers.
    constexpr auto not_keywords = std::to_array<std::string_view>({
        "if",
        "IF",
        "elseif",
        "Get",
        "Set",
        "Step",
        "Lib",
        "From",
        "Weak",
        "Ifx",
        "Els",
        "",
        "Zzz",
    });

    for (const std::string_view text : not_keywords) {
        INFO(text);
        CHECK_FALSE(keyword_kind(text).has_value());
    }
}

TEST_CASE("describe names kinds for diagnostics", "[token]") {
    CHECK(describe(token_kind_t::END_OF_FILE) == "end of file");
    CHECK(describe(token_kind_t::IDENTIFIER) == "identifier");
    CHECK(describe(token_kind_t::LEFT_PAREN) == "'('");
    CHECK(describe(token_kind_t::LESS_LESS_EQUAL) == "'<<='");
    CHECK(describe(token_kind_t::KW_IF) == "'If'");
}
