#include "fb/bindgen.hpp"

#include "fb/bindgen_config.hpp"
#include "fb/build_config.hpp"
#include "fb/c_abi.hpp"
#include "fb/process.hpp"
#include "fb/token.hpp"
#include "fb/toolchain.hpp"
#include "fb/types.hpp"
#include "fb/utf8.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <clang-c/CXErrorCode.h>
#include <clang-c/CXString.h>
#include <clang-c/Index.h>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <iterator>
#include <limits>
#include <llvm-c/Core.h>
#include <llvm-c/TargetMachine.h>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace fb {
namespace {

/// A CXString's text, which it then gives back.
std::string text_of(const CXString string) {
    const char* chars = clang_getCString(string);
    std::string text = chars != nullptr ? chars : "";
    clang_disposeString(string);
    return text;
}

std::string spelling_of(const CXCursor cursor) {
    return text_of(clang_getCursorSpelling(cursor));
}

std::string usr_of(const CXCursor cursor) {
    return text_of(clang_getCursorUSR(cursor));
}

/// Where a cursor is in the file it's expanded in, so a declaration that a macro makes is where
/// the macro is used.
struct place_t {
    CXFile file = nullptr;
    unsigned line = 0;
    unsigned column = 0;
    unsigned offset = 0;
};

place_t place_of(const CXCursor cursor) {
    place_t place;
    clang_getExpansionLocation(clang_getCursorLocation(cursor), &place.file, &place.line,
                               &place.column, &place.offset);
    return place;
}

/// Where a cursor is, as "vecmath.h:12:5".
std::string where(const CXCursor cursor) {
    const place_t place = place_of(cursor);
    const std::string path = place.file != nullptr ? text_of(clang_getFileName(place.file)) : "";
    return std::format("{}:{}:{}", to_utf8(path_from_utf8(path).filename()), place.line,
                       place.column);
}

/// Calls `visit` with each child of `parent`, in order.
template <typename Visit>
void each_child(const CXCursor parent, Visit visit) {
    clang_visitChildren(
        parent,
        [](CXCursor cursor, CXCursor /*parent*/, CXClientData data) {
            (*static_cast<Visit*>(data))(cursor);
            return CXChildVisit_Continue;
        },
        &visit);
}

/// A struct, union, or enum that a definition gives, rather than only names.
bool is_definition(const CXCursor cursor) {
    const CXCursorKind kind = clang_getCursorKind(cursor);
    return (kind == CXCursor_StructDecl || kind == CXCursor_UnionDecl ||
            kind == CXCursor_EnumDecl) &&
           clang_isCursorDefinition(cursor) != 0;
}

/// Whether an expression has a comma operator in it, as `(1, 2)` has: no constant of C's has one,
/// though clang works one out.
bool has_comma(const CXCursor expression) {
    bool found = false;
    clang_visitChildren(
        expression,
        [](CXCursor child, CXCursor /*parent*/, CXClientData data) {
            if (clang_getCursorBinaryOperatorKind(child) == CXBinaryOperator_Comma) {
                *static_cast<bool*>(data) = true;
                return CXChildVisit_Break;
            }

            return CXChildVisit_Recurse;
        },
        &found);
    return found;
}

/// Calls `visit` with each of a unit's diagnostics, from one set: libclang makes the set again
/// for each clang_getNumDiagnostics() when notes are among them, which a loop asking for it
/// each time pays for each diagnostic.
template <typename Visit>
void each_diagnostic(CXTranslationUnit unit, Visit visit) {
    CXDiagnosticSet diagnostics = clang_getDiagnosticSetFromTU(unit);
    const unsigned count = clang_getNumDiagnosticsInSet(diagnostics);
    for (unsigned i = 0; i < count; ++i) {
        CXDiagnostic diagnostic = clang_getDiagnosticInSet(diagnostics, i);
        visit(diagnostic);
        clang_disposeDiagnostic(diagnostic);
    }
}

/// An enum's constants, without the attributes among its children.
template <typename Visit>
void each_constant(const CXCursor enumeration, Visit visit) {
    each_child(enumeration, [&](const CXCursor child) {
        if (clang_getCursorKind(child) == CXCursor_EnumConstantDecl) {
            visit(child);
        }
    });
}

struct index_deleter_t {
    void operator()(void* index) const {
        clang_disposeIndex(index);
    }
};

struct unit_deleter_t {
    void operator()(CXTranslationUnit unit) const {
        clang_disposeTranslationUnit(unit);
    }
};

struct result_deleter_t {
    void operator()(void* result) const {
        clang_EvalResult_dispose(result);
    }
};

using unit_t = std::unique_ptr<std::remove_pointer_t<CXTranslationUnit>, unit_deleter_t>;

/// A FutureBlitz type, or why a C type has none, as "a union".
using mapped_t = std::expected<std::string, std::string>;

/// "a" or "an", for a word.
std::string_view article(const std::string_view word) {
    return !word.empty() && std::string_view("AEIOUaeiou").contains(word.front()) ? "an" : "a";
}

/// Names a module can't give its own declarations: FutureBlitz's keywords, and the names of the
/// types it writes, which a declaration of one would clash with or hide.
bool reserved(const std::string_view name) {
    static constexpr std::array<std::string_view, 4> words{ "Ptr", "Void", "Callback", "Func" };
    return keyword_kind(name).has_value() || builtin_type(name).has_value() ||
           std::ranges::contains(words, name);
}

/// A FutureBlitz name for a C name: its letters, digits, and '_', with a '_' for each other
/// character, like a '$' or one that isn't ASCII, and a '_' after it while it's reserved.
std::string valid_name(const std::string_view c_name) {
    std::string name;
    for (const char c : c_name) {
        const auto byte = static_cast<unsigned char>(c);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '_') {
            name += c;
        } else if ((byte & 0xC0U) != 0x80U) {
            // One '_' for a whole UTF-8 character.
            name += '_';
        }
    }

    while (name.empty() || reserved(name)) {
        name += '_';
    }

    return name;
}

/// Names for C names in one scope, none of them alike: one that's a fine name already keeps it,
/// and the others get a '_' after them until no other has theirs.
class namer_t {
public:
    /// Names `c_names` in order, a name of their own each. An empty C name is `fallback`'s for
    /// its place, like "arg1" for a parameter without one.
    std::vector<std::string> name(const std::vector<std::string>& c_names,
                                  const std::string_view fallback = "") {
        std::vector<std::string> names(c_names.size());
        for (std::size_t i = 0; i < c_names.size(); ++i) {
            if (!c_names[i].empty() && valid_name(c_names[i]) == c_names[i] &&
                m_taken.insert(c_names[i]).second) {
                names[i] = c_names[i];
            }
        }

        for (std::size_t i = 0; i < c_names.size(); ++i) {
            if (names[i].empty()) {
                names[i] =
                    take(c_names[i].empty() ? std::format("{}{}", fallback, i + 1) : c_names[i]);
            }
        }

        return names;
    }

    /// A name for one more C name in the scope.
    std::string take(const std::string_view c_name) {
        std::string name = valid_name(c_name);
        while (!m_taken.insert(name).second) {
            name += '_';
        }

        return name;
    }

private:
    std::set<std::string, std::less<>> m_taken;
};

/// A FutureBlitz Const's type and value, as a literal.
struct constant_t {
    std::string type;
    std::string value;
};

/// A literal of an integer Const's value: decimal, or hex past Int64's range. Int64's least
/// value is a subtraction, since its negation isn't a literal.
std::string integer_literal(const std::string_view type, const std::uint64_t bits) {
    if (type == "Bool") {
        return bits != 0 ? "True" : "False";
    }

    if (type.starts_with('U')) {
        return bits > static_cast<std::uint64_t>(INT64_MAX) ? std::format("0x{:X}", bits)
                                                            : std::format("{}", bits);
    }

    const auto value = static_cast<std::int64_t>(bits);
    return value == INT64_MIN ? std::string("-9223372036854775807 - 1") : std::format("{}", value);
}

/// A literal of a float Const's value, the shortest that reads back as it: a Float32's as a
/// float's, so C's 1.0f / 3 is 0.33333334.
std::string float_literal(const bool single, const double value) {
    std::array<char, 64> buffer{};
    char* const end = buffer.data() + buffer.size();
    const std::to_chars_result written =
        single ? std::to_chars(buffer.data(), end, static_cast<float>(value))
               : std::to_chars(buffer.data(), end, value);
    std::string text(buffer.data(), written.ptr);
    if (const std::size_t plus = text.find("e+"); plus != std::string::npos) {
        text.erase(plus + 1, 1);
    }

    if (text.find_first_of(".e") == std::string::npos) {
        text += ".0";
    }

    return text;
}

/// A C struct the header defines, by its USR.
struct record_t {
    CXCursor definition = clang_getNullCursor();
    /// Its FutureBlitz name: a typedef's for it, or else its tag. Empty when it has neither.
    std::string name;
    /// Its C name, the same way.
    std::string c_name;
    bool checked = false;
    /// Why it can't be a @CLayout Struct, once checked.
    std::optional<std::string> problem;
};

/// A C enum the header defines: an Enum named `name`, or Consts when `name` is empty.
struct enumeration_t {
    std::string c_name;
    std::string name;
    std::string integer;
    /// It's Consts: its values repeat, or nothing names its type.
    bool constants = false;
    /// It has neither a tag nor a typedef, so nothing names its type.
    bool anonymous = false;
};

/// A C function the header declares, maybe more than once.
struct function_t {
    /// Where it's bound: its first declaration.
    CXCursor first = clang_getNullCursor();
    /// What's bound: its last declaration that says what it takes, or its first.
    CXCursor declaration = clang_getNullCursor();
    /// Every declaration, in order, whose parameters may have names where the bound one's don't.
    std::vector<CXCursor> declarations;
    std::string name;
};

/// An object-like or function-like macro the header defines, maybe more than once.
struct macro_t {
    /// Where its Const goes: its last definition, which is what C sees after the header.
    CXCursor last = clang_getNullCursor();
    /// Its Const, or why it has none: empty for a macro that stands for nothing new, like an
    /// include guard.
    std::expected<constant_t, std::string> value = std::unexpected(std::string());
    std::string name;
};

/// One part of the module, in the header's order. Single lines, like Consts, go together
/// without blank lines between them.
struct block_t {
    std::vector<std::string> lines;
    bool single = false;
};

class generator_t {
public:
    generator_t(const bindgen_options_t& options, CXIndex index,
                const std::vector<std::string>& arguments, CXTranslationUnit unit)
        : m_options(options), m_index(index), m_arguments(arguments), m_unit(unit),
          m_main(clang_getFile(unit, text_of(clang_getTranslationUnitSpelling(unit)).c_str())) {}

    bindings_t run() {
        each_child(clang_getTranslationUnitCursor(m_unit), [this](const CXCursor cursor) {
            if (in_main_file(cursor)) {
                m_items.push_back(cursor);
            }
        });

        // libclang gives the macros first, so they're put in the header's order.
        std::ranges::stable_sort(m_items, {},
                                 [](const CXCursor cursor) { return place_of(cursor).offset; });
        for (const CXCursor item : m_items) {
            if (is_definition(item)) {
                gather(item);
            }
        }

        read_macros();
        name_everything();
        decide_records();
        for (const CXCursor item : m_items) {
            emit_item(item);
        }

        return { .text = assemble(), .warnings = std::move(m_warnings) };
    }

private:
    const bindgen_options_t& m_options;
    CXIndex m_index;
    const std::vector<std::string>& m_arguments;
    CXTranslationUnit m_unit;
    /// The header, which declarations are in when they're its own.
    CXFile m_main;
    std::vector<CXCursor> m_items;
    /// The header's struct, union, and enum definitions, those inside others too, each before
    /// what it holds.
    std::vector<CXCursor> m_definitions;
    /// The header's structs and enums by USR.
    std::map<std::string, record_t, std::less<>> m_records;
    std::map<std::string, enumeration_t, std::less<>> m_enums;
    /// The FutureBlitz names of the Consts of enums that aren't Enums, by the constants' USRs.
    std::map<std::string, std::string, std::less<>> m_constant_names;
    std::map<std::string, function_t, std::less<>> m_functions;
    std::map<std::string, macro_t, std::less<>> m_macros;
    /// Every macro clang saw, the header's, those of what it includes, and -D's, by name, each
    /// by its last definition.
    std::map<std::string, CXCursor, std::less<>> m_every_macro;
    /// Whether each macro looked at could stand for a number.
    std::map<std::string, bool, std::less<>> m_could_be_numbers;
    /// What the names of the file clang works out macros in start with, which no macro's does.
    std::string m_prefix;
    /// The C names typedefs give the header's structs, unions, and enums, by their USRs.
    std::map<std::string, std::string, std::less<>> m_typedefs;
    /// While records are checked, a pointer is a Ptr<Void> whatever it points at, since what a
    /// struct points at never decides whether it's a @CLayout Struct.
    bool m_deciding = false;
    /// The definitions written so far, by USR.
    std::set<std::string, std::less<>> m_emitted;
    std::vector<block_t> m_blocks;
    /// The Extern block's lines: Functions, and comments for what's skipped.
    std::vector<std::string> m_externs;
    std::vector<std::string> m_warnings;

    /// Whether a declaration is the header's own: written in it, or made by a macro it uses.
    [[nodiscard]] bool in_main_file(const CXCursor cursor) const {
        const place_t place = place_of(cursor);
        return place.file != nullptr && clang_File_isEqual(place.file, m_main) != 0;
    }

    void gather(const CXCursor definition) {
        m_definitions.push_back(definition);
        each_child(definition, [this](const CXCursor child) {
            if (is_definition(child)) {
                gather(child);
            }
        });
    }

    // Macros

    /// The tokens after a macro's name.
    std::vector<std::string> body_of(const CXCursor macro) {
        CXToken* tokens = nullptr;
        unsigned count = 0;
        clang_tokenize(m_unit, clang_getCursorExtent(macro), &tokens, &count);
        std::vector<std::string> body;
        for (unsigned i = 1; i < count; ++i) {
            body.push_back(text_of(clang_getTokenSpelling(m_unit, tokens[i])));
        }

        clang_disposeTokens(m_unit, tokens, count);
        return body;
    }

    /// Whether a macro's body could be an expression of C's: nothing that ends or opens a
    /// statement, its brackets matched, no comma between two values, and no "::", which C has
    /// no use for and clang's recovery from never ends.
    static bool could_be_expression(const std::span<const std::string> body) {
        static constexpr std::array<std::string_view, 26> refused{
            "{",        "}",      ";",    "#",       "##",      "::",     "if",
            "else",     "while",  "do",   "for",     "return",  "goto",   "break",
            "continue", "switch", "case", "default", "typedef", "extern", "static",
            "register", "inline", "auto", "asm",     "__asm__",
        };
        int depth = 0;
        for (std::size_t i = 0; i < body.size(); ++i) {
            const std::string& token = body[i];
            // C may read "::" as two colons, which no expression has either.
            const bool colons = token == ":" && i + 1 < body.size() && body[i + 1] == ":";
            if (std::ranges::contains(refused, token) || colons || (token == "," && depth == 0)) {
                return false;
            }

            if (token == "(" || token == "[") {
                ++depth;
            } else if (token == ")" || token == "]") {
                --depth;
            }

            if (depth < 0) {
                return false;
            }
        }

        return depth == 0;
    }

    /// Whether a macro could stand for a number: its body could be an expression, and so could
    /// those of the macros it uses, as C expands them.
    bool could_be_number(const std::string& name) {
        if (const auto found = m_could_be_numbers.find(name); found != m_could_be_numbers.end()) {
            return found->second;
        }

        const auto macro = m_every_macro.find(name);
        if (macro == m_every_macro.end() || clang_Cursor_isMacroBuiltin(macro->second) != 0) {
            return true;
        }

        // One that uses itself stands for its name there, since C doesn't expand it again.
        m_could_be_numbers[name] = true;
        const std::vector<std::string> body = body_of(macro->second);
        auto from = body.begin();
        // A function-like macro's parameters come first, with their commas.
        if (clang_Cursor_isMacroFunctionLike(macro->second) != 0) {
            from = std::ranges::find(body, ")");
            from = from == body.end() ? from : from + 1;
        }

        const std::span<const std::string> expression(from, body.end());
        const bool could = could_be_expression(expression) &&
                           std::ranges::all_of(expression, [this](const std::string& token) {
                               return !m_every_macro.contains(token) || could_be_number(token);
                           });
        m_could_be_numbers[name] = could;
        return could;
    }

    /// Reads the header's macros, each by its last definition, and has clang work out the ones
    /// that could be numbers as C would after the header, so each has C's type and value.
    void read_macros() {
        each_child(clang_getTranslationUnitCursor(m_unit), [this](const CXCursor cursor) {
            if (clang_getCursorKind(cursor) == CXCursor_MacroDefinition) {
                m_every_macro.insert_or_assign(spelling_of(cursor), cursor);
            }
        });
        // The names clang works them out in start the way no macro's does.
        m_prefix = "__fb_bindgen_";
        while (std::ranges::any_of(m_every_macro, [this](const auto& macro) {
            return macro.first.starts_with(m_prefix);
        })) {
            m_prefix += '_';
        }

        for (const CXCursor item : m_items) {
            if (clang_getCursorKind(item) == CXCursor_MacroDefinition &&
                clang_Cursor_isMacroBuiltin(item) == 0) {
                m_macros[spelling_of(item)].last = item;
            }
        }

        std::vector<std::string> defined;
        std::vector<std::string> numbers;
        for (auto& [name, macro] : m_macros) {
            const std::vector<std::string> body = body_of(macro.last);
            if (clang_Cursor_isMacroFunctionLike(macro.last) != 0) {
                macro.value = std::unexpected(std::format("{}, a function-like macro", name));
            } else if (body.empty() || body == std::vector{ name }) {
                // An include guard, a flag, or "#define X X" beside X, which stand for nothing.
                continue;
            } else {
                macro.value = std::unexpected(std::format("{}, a macro that isn't a number", name));
                defined.push_back(name);
                if (could_be_number(name)) {
                    numbers.push_back(name);
                }
            }
        }

        evaluate(defined, numbers);
    }

    /// Has clang work out the header's macros in a file of its own that includes it: whether
    /// each of `defined` still is after the header, and each of `numbers` as a variable of its
    /// type, `__typeof__((NAME)) v = (NAME);`, twice, the second after a #line, so a value that
    /// depends on where it's used, like __LINE__'s, shows. What isn't a number known at compile
    /// time is an error, which leaves the macro skipped, as is one that's a warning clang
    /// recovers from, like a comma's unused left side. An integer is one only where C takes an
    /// integer constant expression too, in a _Static_assert that holds whatever its value, which
    /// what C leaves undefined isn't, like INT_MIN / -1 or (int)1e10, though clang folds them in
    /// a variable's value.
    void evaluate(const std::vector<std::string>& defined,
                  const std::vector<std::string>& numbers) {
        if (defined.empty()) {
            return;
        }

        std::string text;
        std::size_t line = 1;
        const auto add = [&](const std::string& code) {
            text += code;
            line += static_cast<std::size_t>(std::ranges::count(code, '\n'));
        };
        for (std::size_t i = 0; i < defined.size(); ++i) {
            add(std::format("#ifndef {}\nint {}gone_{};\n#endif\n", defined[i], m_prefix, i));
        }

        // The line each macro's variable starts at, so an error says which macro it's in, and
        // the line each one's _Static_assert does.
        std::map<std::size_t, std::size_t> starts;
        for (const std::string_view copy : { "value", "again" }) {
            if (copy == "again") {
                add("#line 1000000 \"fb-bindgen-again.c\"\n");
            }

            for (std::size_t i = 0; i < numbers.size(); ++i) {
                starts[line] = i;
                add(std::format("#ifdef {0}\n__typeof__(({0})) {1}{2}_{3} = ({0});\n#endif\n",
                                numbers[i], m_prefix, copy, i));
            }
        }

        const std::size_t asserts = line;
        std::map<std::size_t, std::size_t> assert_starts;
        for (std::size_t i = 0; i < numbers.size(); ++i) {
            assert_starts[line] = i;
            add(std::format("#ifdef {0}\n_Static_assert(({0}) * 0 + 1, \"\");\n#endif\n",
                            numbers[i]));
        }

        std::filesystem::path probe = m_options.header;
        probe.replace_extension(".fb-bindgen-values.c");
        const std::string probe_path = to_utf8(probe);
        CXUnsavedFile unsaved{
            .Filename = probe_path.c_str(),
            .Contents = text.c_str(),
            .Length = static_cast<unsigned long>(text.size()),
        };
        // What C leaves undefined, like an overflow, is an error too, so it has no value, and
        // every error counts.
        std::vector<std::string> arguments = m_arguments;
        arguments.insert(arguments.end(), {
                                              "-include",
                                              to_utf8(m_options.header),
                                              "-ferror-limit=0",
                                              "-Werror=unused-value",
                                              "-Werror=integer-overflow",
                                              "-Werror=shift-count-negative",
                                              "-Werror=shift-count-overflow",
                                              "-Werror=shift-negative-value",
                                              "-Werror=shift-overflow",
                                              "-Werror=division-by-zero",
                                              "-Werror=gnu-folding-constant",
                                          });
        std::vector<const char*> argv;
        argv.reserve(arguments.size());
        for (const std::string& argument : arguments) {
            argv.push_back(argument.c_str());
        }

        CXTranslationUnit parsed = nullptr;
        (void)clang_parseTranslationUnit2(
            m_index, probe_path.c_str(), argv.data(), static_cast<int>(argv.size()), &unsaved, 1,
            static_cast<unsigned>(CXTranslationUnit_SkipFunctionBodies), &parsed);
        const unit_t unit(parsed);
        if (!unit) {
            return;
        }

        // An error in a macro's expansion is where the macro's used. One in its _Static_assert
        // counts only for an integer, since a floating-point number is never one C takes there.
        CXFile file = clang_getFile(unit.get(), probe_path.c_str());
        std::set<std::size_t> failed;
        std::set<std::size_t> not_constant;
        each_diagnostic(unit.get(), [&](CXDiagnostic diagnostic) {
            CXFile in = nullptr;
            unsigned at = 0;
            clang_getExpansionLocation(clang_getDiagnosticLocation(diagnostic), &in, &at, nullptr,
                                       nullptr);
            if (clang_getDiagnosticSeverity(diagnostic) < CXDiagnostic_Error || in == nullptr ||
                clang_File_isEqual(in, file) == 0) {
                return;
            }

            const std::map<std::size_t, std::size_t>& lines =
                at >= asserts ? assert_starts : starts;
            if (const auto start = lines.upper_bound(at); start != lines.begin()) {
                (at >= asserts ? not_constant : failed).insert(std::prev(start)->second);
            }
        });

        std::set<std::size_t> gone;
        std::map<std::size_t, CXCursor> values;
        std::map<std::size_t, CXCursor> again;
        each_child(clang_getTranslationUnitCursor(unit.get()), [&](const CXCursor cursor) {
            const std::string spelling = spelling_of(cursor);
            if (clang_getCursorKind(cursor) != CXCursor_VarDecl ||
                !spelling.starts_with(m_prefix) ||
                clang_Location_isFromMainFile(clang_getCursorLocation(cursor)) == 0) {
                return;
            }

            // "value_3": which copy, and which macro.
            const std::string_view rest = std::string_view(spelling).substr(m_prefix.size());
            const std::size_t mark = rest.find('_');
            std::size_t at = 0;
            if (mark == std::string_view::npos ||
                std::from_chars(rest.data() + mark + 1, rest.data() + rest.size(), at).ec !=
                    std::errc()) {
                return;
            }

            const std::string_view copy = rest.substr(0, mark);
            if (copy == "gone" && at < defined.size()) {
                gone.insert(at);
            } else if (copy == "value") {
                values.emplace(at, cursor);
            } else if (copy == "again") {
                again.emplace(at, cursor);
            }
        });
        for (const std::size_t at : gone) {
            m_macros.at(defined[at]).value =
                std::unexpected(std::format("{}, a macro the header #undefs", defined[at]));
        }

        for (std::size_t i = 0; i < numbers.size(); ++i) {
            const auto first = values.find(i);
            const auto second = again.find(i);
            if (failed.contains(i) || first == values.end() || second == again.end()) {
                continue;
            }

            std::expected<constant_t, std::string> value = constant_of(first->second, numbers[i]);
            if (value && !value->type.starts_with("Float") && not_constant.contains(i)) {
                continue;
            }

            const std::expected<constant_t, std::string> moved =
                constant_of(second->second, numbers[i]);
            if (value && (!moved || value->type != moved->type || value->value != moved->value)) {
                value = std::unexpected(std::format(
                    "{}, a macro whose value depends on where it's used, like __LINE__'s",
                    numbers[i]));
            }

            m_macros.at(numbers[i]).value = std::move(value);
        }
    }

    /// Whether every target's CLong, CULong, or CWChar holds a value of the target's, by its
    /// bits: an Int32's, a UInt32's, or a UInt16's.
    static bool fits_everywhere(const std::string_view type, const std::uint64_t bits) {
        const auto value = static_cast<std::int64_t>(bits);
        if (type == "CLong") {
            return value >= INT32_MIN && value <= INT32_MAX;
        }

        if (type == "CULong") {
            return bits <= std::numeric_limits<std::uint32_t>::max();
        }

        return bits <= std::numeric_limits<std::uint16_t>::max();
    }

    /// The Const for a macro that clang worked out as `variable`, or why it has none.
    std::expected<constant_t, std::string> constant_of(const CXCursor variable,
                                                       const std::string& name) const {
        const auto skipped = [&](const std::string_view why) {
            return std::unexpected(std::format("{}, a macro {}", name, why));
        };
        if (clang_isInvalidDeclaration(variable) != 0 || has_comma(variable)) {
            return skipped("that isn't a number");
        }

        const CXType type = clang_getCanonicalType(clang_getCursorType(variable));
        std::string fb_type;
        switch (type.kind) {
        case CXType_Bool:
            fb_type = "Bool";
            break;
        case CXType_Float:
            fb_type = "Float32";
            break;
        case CXType_Double:
            fb_type = "Float64";
            break;
        case CXType_LongDouble:
            return skipped("of a long double");
        case CXType_Int128:
        case CXType_UInt128:
            return skipped("of a 128-bit integer");
        case CXType_Enum:
            fb_type = integer_type(clang_getEnumDeclIntegerType(clang_getTypeDeclaration(type)))
                          .value_or("");
            break;
        default:
            fb_type = is_integer(type) ? integer_type(type).value_or("") : "";
            break;
        }

        const std::unique_ptr<void, result_deleter_t> result(clang_Cursor_Evaluate(variable));
        if (fb_type.empty() || !result) {
            return skipped("that isn't a number");
        }

        switch (clang_EvalResult_getKind(result.get())) {
        case CXEval_Int: {
            const bool is_unsigned = clang_EvalResult_isUnsignedInt(result.get()) != 0;
            const std::uint64_t bits =
                is_unsigned
                    ? clang_EvalResult_getAsUnsigned(result.get())
                    : static_cast<std::uint64_t>(clang_EvalResult_getAsLongLong(result.get()));
            // One of C's own integers, where every target's holds the value. The variable's
            // __typeof__ hides the names its type was written with, which its value keeps.
            CXType written = clang_getCursorType(variable);
            each_child(variable, [&](const CXCursor child) {
                if (clang_isExpression(clang_getCursorKind(child)) != 0) {
                    written = clang_getCursorType(child);
                }
            });
            const std::optional<std::string> integer = wide_character(variable)
                                                           ? std::optional<std::string>("CWChar")
                                                           : c_integer(written);
            if (integer && fits_everywhere(*integer, bits)) {
                return constant_t{ .type = *integer, .value = integer_literal(*integer, bits) };
            }

            return constant_t{ .type = fb_type, .value = integer_literal(fb_type, bits) };
        }
        case CXEval_Float: {
            const double value = clang_EvalResult_getAsDouble(result.get());
            if (!std::isfinite(value)) {
                return skipped("that isn't a finite number");
            }

            return constant_t{
                .type = fb_type,
                .value = float_literal(fb_type == "Float32", value),
            };
        }
        default:
            return skipped("that isn't a number");
        }
    }

    // Names

    /// Names what C names in its ordinary namespace first, functions and the Consts of macros
    /// and enums, then structs and enums, whose tags are apart in C, so one of a taken name
    /// gets a '_'. A C name that's a fine name already keeps it.
    void name_everything() {
        for (const CXCursor item : m_items) {
            if (clang_getCursorKind(item) == CXCursor_TypedefDecl) {
                const CXType underlying =
                    clang_getCanonicalType(clang_getTypedefDeclUnderlyingType(item));
                if (underlying.kind == CXType_Record || underlying.kind == CXType_Enum) {
                    m_typedefs.try_emplace(usr_of(clang_getTypeDeclaration(underlying)),
                                           spelling_of(item));
                }
            } else if (clang_getCursorKind(item) == CXCursor_FunctionDecl) {
                const auto [found, added] = m_functions.try_emplace(
                    spelling_of(item), function_t{ .first = item, .declaration = item });
                // A later declaration that says what it takes is what's bound.
                if (!added && clang_getCursorType(item).kind == CXType_FunctionProto) {
                    found->second.declaration = item;
                }

                found->second.declarations.push_back(item);
            }
        }

        for (const CXCursor definition : m_definitions) {
            add_type(definition);
        }

        name_field_structs();

        // The ordinary names, in the header's order.
        std::vector<std::string> ordinary;
        std::vector<std::string*> targets;
        const auto add = [&](const std::string& c_name, std::string& name) {
            ordinary.push_back(c_name);
            targets.push_back(&name);
        };
        for (const CXCursor item : m_items) {
            const CXCursorKind kind = clang_getCursorKind(item);
            const std::string c_name = spelling_of(item);
            if (kind == CXCursor_FunctionDecl &&
                clang_equalCursors(m_functions.at(c_name).first, item) != 0) {
                add(c_name, m_functions.at(c_name).name);
            } else if (kind == CXCursor_MacroDefinition && m_macros.contains(c_name) &&
                       clang_equalCursors(m_macros.at(c_name).last, item) != 0 &&
                       m_macros.at(c_name).value) {
                add(c_name, m_macros.at(c_name).name);
            } else if (is_definition(item)) {
                add_constants(item, add);
            }
        }

        namer_t namer;
        const std::vector<std::string> names = namer.name(ordinary);
        for (std::size_t i = 0; i < names.size(); ++i) {
            *targets[i] = names[i];
        }

        // Then the structs and enums.
        std::vector<std::string> tags;
        std::vector<std::string*> named;
        for (const CXCursor definition : m_definitions) {
            const std::string usr = usr_of(definition);
            if (const auto record = m_records.find(usr);
                record != m_records.end() && !record->second.c_name.empty()) {
                tags.push_back(record->second.c_name);
                named.push_back(&record->second.name);
            } else if (const auto enumeration = m_enums.find(usr);
                       enumeration != m_enums.end() && !enumeration->second.constants) {
                tags.push_back(enumeration->second.c_name);
                named.push_back(&enumeration->second.name);
            }
        }

        const std::vector<std::string> tag_names = namer.name(tags);
        for (std::size_t i = 0; i < tag_names.size(); ++i) {
            *named[i] = tag_names[i];
        }
    }

    /// Names a struct without a name that's a field's type, as in "struct holder { struct { int
    /// a; } inner; }", after the struct and the field: holder_inner. A struct comes before what it
    /// holds, so one inside another without a name gets its name first.
    void name_field_structs() {
        for (const CXCursor definition : m_definitions) {
            const auto outer = m_records.find(usr_of(definition));
            if (outer == m_records.end() || outer->second.c_name.empty()) {
                continue;
            }

            each_child(definition, [&](const CXCursor field) {
                if (clang_getCursorKind(field) != CXCursor_FieldDecl) {
                    return;
                }

                CXType type = clang_getCanonicalType(clang_getCursorType(field));
                while (type.kind == CXType_ConstantArray) {
                    type = clang_getCanonicalType(clang_getArrayElementType(type));
                }

                const auto inner = type.kind == CXType_Record
                                       ? m_records.find(usr_of(clang_getTypeDeclaration(type)))
                                       : m_records.end();
                if (inner != m_records.end() && inner->second.c_name.empty()) {
                    inner->second.c_name =
                        std::format("{}_{}", outer->second.c_name, spelling_of(field));
                }
            });
        }
    }

    /// Adds the Consts of an enum that isn't an Enum, and of those defined inside a struct or
    /// union, to the ordinary names.
    template <typename Add>
    void add_constants(const CXCursor definition, Add& add) {
        if (clang_getCursorKind(definition) == CXCursor_EnumDecl) {
            if (m_enums.at(usr_of(definition)).constants) {
                each_constant(definition, [&](const CXCursor constant) {
                    add(spelling_of(constant), m_constant_names[usr_of(constant)]);
                });
            }

            return;
        }

        each_child(definition, [&](const CXCursor child) {
            if (is_definition(child)) {
                add_constants(child, add);
            }
        });
    }

    /// A struct's, union's, or enum's C name: a typedef's for it, or else its tag. Empty when it
    /// has neither.
    std::string c_name_of(const CXCursor definition) {
        if (const auto named = m_typedefs.find(usr_of(definition)); named != m_typedefs.end()) {
            return named->second;
        }

        return clang_Cursor_isAnonymous(definition) == 0 ? spelling_of(definition) : "";
    }

    void add_type(const CXCursor definition) {
        const std::string usr = usr_of(definition);
        const CXCursorKind kind = clang_getCursorKind(definition);
        if (kind == CXCursor_StructDecl) {
            m_records.try_emplace(
                usr, record_t{ .definition = definition, .c_name = c_name_of(definition) });
            return;
        }

        if (kind != CXCursor_EnumDecl) {
            return;
        }

        // An enum whose values repeat, that has no name, or whose values are flags to combine,
        // which an Enum's value can't be, is Consts of its integer type.
        std::set<std::uint64_t> values;
        bool repeats = false;
        each_constant(definition, [&](const CXCursor constant) {
            const auto bits = static_cast<std::uint64_t>(clang_getEnumConstantDeclValue(constant));
            repeats = repeats || !values.insert(bits).second;
        });
        each_child(definition, [&](const CXCursor child) {
            repeats = repeats || clang_getCursorKind(child) == CXCursor_FlagEnum;
        });
        const std::string c_name = c_name_of(definition);
        m_enums.try_emplace(
            usr,
            enumeration_t{
                .c_name = c_name,
                .integer = integer_type(clang_getEnumDeclIntegerType(definition)).value_or("Int32"),
                .constants = repeats || c_name.empty() || values.empty(),
                .anonymous = c_name.empty(),
            });
    }

    // Types

    static bool is_integer(const CXType type) {
        switch (clang_getCanonicalType(type).kind) {
        case CXType_Char_S:
        case CXType_SChar:
        case CXType_Short:
        case CXType_Int:
        case CXType_Long:
        case CXType_LongLong:
        case CXType_Char_U:
        case CXType_UChar:
        case CXType_UShort:
        case CXType_UInt:
        case CXType_ULong:
        case CXType_ULongLong:
        case CXType_Char16:
        case CXType_Char32:
        case CXType_WChar:
            return true;
        default:
            return false;
        }
    }

    /// An integer type of the C type's size and signedness.
    static mapped_t integer_type(const CXType type) {
        const CXType canonical = clang_getCanonicalType(type);
        bool is_signed = false;
        switch (canonical.kind) {
        case CXType_Char_S:
        case CXType_SChar:
        case CXType_Short:
        case CXType_Int:
        case CXType_Long:
        case CXType_LongLong:
            is_signed = true;
            break;
        case CXType_WChar:
            // Windows' wchar_t is an unsigned 16 bits, and others' a signed 32.
            is_signed = clang_Type_getSizeOf(canonical) == 4;
            break;
        default:
            break;
        }

        switch (clang_Type_getSizeOf(canonical)) {
        case 1:
            return is_signed ? "Int8" : "UInt8";
        case 2:
            return is_signed ? "Int16" : "UInt16";
        case 4:
            return is_signed ? "Int32" : "UInt32";
        case 8:
            return is_signed ? "Int64" : "UInt64";
        default:
            return std::unexpected("an integer of a size FutureBlitz hasn't");
        }
    }

    /// `type` with the name or attribute it was written with taken off, one at a time: a
    /// typedef's type, or what an elaborated or attributed type names. nullopt for one without.
    static std::optional<CXType> peeled(const CXType type) {
        switch (type.kind) {
        case CXType_Elaborated:
            return clang_Type_getNamedType(type);
        case CXType_Typedef:
            return clang_getTypedefDeclUnderlyingType(clang_getTypeDeclaration(type));
        case CXType_Attributed:
            return clang_Type_getModifiedType(type);
        default:
            return std::nullopt;
        }
    }

    /// `type` as a type of `kind`, as written, through its names, or else its canonical type.
    static CXType written_as(CXType type, const CXTypeKind kind) {
        while (type.kind != kind) {
            const std::optional<CXType> inner = peeled(type);
            if (!inner) {
                return clang_getCanonicalType(type);
            }

            type = *inner;
        }

        return type;
    }

    /// Whether a typedef's name says how big its integer is, as stdint.h's and stddef.h's do,
    /// like `int64_t` and `size_t`, which are a `long` on some targets.
    static bool sized_by_name(std::string_view name) {
        constexpr auto sized = std::to_array<std::string_view>({
            "size_t",
            "ssize_t",
            "ptrdiff_t",
            "intptr_t",
            "uintptr_t",
            "intmax_t",
            "uintmax_t",
            "char16_t",
            "char32_t",
        });
        if (std::ranges::contains(sized, name)) {
            return true;
        }

        // intN_t, int_leastN_t, int_fastN_t, and their unsigned ones.
        if (name.starts_with('u')) {
            name.remove_prefix(1);
        }

        if (!name.starts_with("int") || !name.ends_with("_t")) {
            return false;
        }

        name = name.substr(3, name.size() - 5);
        if (name.starts_with("_least")) {
            name.remove_prefix(6);
        } else if (name.starts_with("_fast")) {
            name.remove_prefix(5);
        }

        return name == "8" || name == "16" || name == "32" || name == "64";
    }

    /// CLong, CULong, or CWChar for a type written as C's `long`, `unsigned long`, or `wchar_t`,
    /// through typedefs, but not through one whose name gives its integer's size, nor a header's
    /// own `wchar_t` that isn't the target's.
    [[nodiscard]] std::optional<std::string> c_integer(CXType type) const {
        while (true) {
            if (type.kind == CXType_Typedef) {
                const std::string name = text_of(clang_getTypedefName(type));
                if (name == "wchar_t" && is_target_wchar(type)) {
                    return "CWChar";
                }

                if (sized_by_name(name)) {
                    return std::nullopt;
                }
            }

            const std::optional<CXType> inner = peeled(type);
            if (!inner) {
                break;
            }

            type = *inner;
        }

        switch (type.kind) {
        case CXType_Long:
            return "CLong";
        case CXType_ULong:
            return "CULong";
        case CXType_WChar:
            return "CWChar";
        default:
            return std::nullopt;
        }
    }

    /// Whether a type is of the size and sign of the target's wchar_t, as stddef.h's is and a
    /// header's own typedef of that name may not be.
    [[nodiscard]] bool is_target_wchar(const CXType type) const {
        const type_id_t wchar = on_target(type_id_t::CWCHAR, c_integers_of(target()));
        const CXType canonical = clang_getCanonicalType(type);
        return clang_Type_getSizeOf(canonical) == bit_width(wchar) / 8 &&
               signed_kind(canonical.kind) == is_signed(wchar);
    }

    /// Whether an integer kind is a signed one.
    static bool signed_kind(const CXTypeKind kind) {
        switch (kind) {
        case CXType_Char_S:
        case CXType_SChar:
        case CXType_Short:
        case CXType_Int:
        case CXType_Long:
        case CXType_LongLong:
            return true;
        default:
            return false;
        }
    }

    /// Whether a macro's value, the variable `variable` that clang worked it out as, is a wide
    /// character literal, like L'/', which C gives the type wchar_t, though clang gives it the
    /// target's integer.
    static bool wide_character(const CXCursor variable) {
        bool wide = false;
        clang_visitChildren(
            variable,
            [](const CXCursor child, CXCursor /*parent*/, CXClientData data) {
                const CXCursorKind kind = clang_getCursorKind(child);
                if (kind != CXCursor_CharacterLiteral) {
                    return kind == CXCursor_ParenExpr || kind == CXCursor_UnexposedExpr
                               ? CXChildVisit_Recurse
                               : CXChildVisit_Continue;
                }

                // Its text is where it's spelled, in the macro's definition, not where the
                // macro's name is.
                CXFile file = nullptr;
                unsigned offset = 0;
                clang_getSpellingLocation(clang_getRangeStart(clang_getCursorExtent(child)), &file,
                                          nullptr, nullptr, &offset);
                std::size_t size = 0;
                const char* text =
                    file != nullptr
                        ? clang_getFileContents(clang_Cursor_getTranslationUnit(child), file, &size)
                        : nullptr;
                *static_cast<bool*>(data) = text != nullptr && offset + 1 < size &&
                                            text[offset] == 'L' && text[offset + 1] == '\'';
                return CXChildVisit_Break;
            },
            &wide);
        return wide;
    }

    record_t* record_of(const CXType type) {
        const auto found = m_records.find(usr_of(clang_getTypeDeclaration(type)));
        return found != m_records.end() ? &found->second : nullptr;
    }

    /// Whether a struct is a @CLayout Struct. decide_records() finds out for each first.
    bool usable(record_t& record) {
        if (!record.checked) {
            record.checked = true;
            record.problem = problem_of(record);
        }

        return !record.problem;
    }

    /// Decides which structs are @CLayout Structs before anything's written, so what holds one
    /// by value knows, and what points at one does too.
    void decide_records() {
        m_deciding = true;
        for (auto& [usr, record] : m_records) {
            (void)usable(record);
        }

        m_deciding = false;
    }

    /// What `written` maps to, by its canonical type, but for C's own integers, which only its
    /// names say.
    mapped_t map_type(const CXType written) {
        if (std::optional<std::string> integer = c_integer(written)) {
            return std::move(*integer);
        }

        const CXType type = clang_getCanonicalType(written);
        switch (type.kind) {
        case CXType_Bool:
            return "Bool";
        case CXType_Float:
            return "Float32";
        case CXType_Double:
            return "Float64";
        case CXType_LongDouble:
            return std::unexpected("a long double");
        case CXType_Int128:
        case CXType_UInt128:
            return std::unexpected("a 128-bit integer");
        case CXType_Pointer:
            return map_pointee(clang_getPointeeType(written_as(written, CXType_Pointer)));
        case CXType_Record:
            return map_record(type);
        case CXType_Enum:
            return map_enum(type);
        case CXType_ConstantArray:
            return map_array(written);
        case CXType_IncompleteArray:
            return std::unexpected("an array without a size");
        case CXType_Void:
            return std::unexpected("void");
        default: {
            if (is_integer(type)) {
                return integer_type(type);
            }

            const std::string kind = text_of(clang_getTypeKindSpelling(type.kind));
            return std::unexpected(std::format("{} {}", article(kind), kind));
        }
        }
    }

    /// A pointer at `written`: `const char*` is a CString, a function pointer a Callback, and
    /// `void*`, a pointer to a struct that isn't a @CLayout Struct here, like an opaque
    /// handle's, or to what has no type is a Ptr<Void>.
    mapped_t map_pointee(const CXType written) {
        const CXType pointee = clang_getCanonicalType(written);
        if ((pointee.kind == CXType_Char_S || pointee.kind == CXType_Char_U) &&
            clang_isConstQualifiedType(pointee) != 0) {
            return "CString";
        }

        if (pointee.kind == CXType_FunctionProto) {
            return map_callback(written_as(written, CXType_FunctionProto));
        }

        if (pointee.kind == CXType_FunctionNoProto) {
            return map_callback(pointee);
        }

        if (m_deciding) {
            return "Ptr<Void>";
        }

        if (pointee.kind == CXType_Record) {
            record_t* record = record_of(pointee);
            return record != nullptr && usable(*record) ? std::format("Ptr<{}>", record->name)
                                                        : std::string("Ptr<Void>");
        }

        const mapped_t inner = map_type(written);
        return inner ? std::format("Ptr<{}>", *inner) : std::string("Ptr<Void>");
    }

    /// A parameter as C passes it, an array as a pointer at its element, from its type as it's
    /// declared, whose names say which integers are C's own. The canonical type stands in for
    /// one libclang doesn't give.
    mapped_t map_parameter(const CXType declared, const CXType canonical) {
        if (declared.kind == CXType_Invalid) {
            return map_type(canonical);
        }

        const CXType type = clang_getCanonicalType(declared);
        if (type.kind == CXType_ConstantArray || type.kind == CXType_IncompleteArray ||
            type.kind == CXType_VariableArray) {
            return map_pointee(clang_getArrayElementType(written_as(declared, type.kind)));
        }

        // A function goes as a pointer to it too, as in `int compare(int a, int b)`.
        if (type.kind == CXType_FunctionProto || type.kind == CXType_FunctionNoProto) {
            return map_pointee(declared);
        }

        return map_type(declared);
    }

    /// The calling convention a function type is declared with, like "vectorcall", when it
    /// isn't the target's C convention, which is the only one FutureBlitz calls and is called
    /// by. It's empty for one without a name here.
    [[nodiscard]] std::optional<std::string> convention_of(const CXType function) const {
        const CXCallingConv convention = clang_getFunctionTypeCallingConv(function);
        const c_abi_t abi = c_abi_of(target());
        if (convention == CXCallingConv_C ||
            (convention == CXCallingConv_Win64 && abi == c_abi_t::WIN64) ||
            (convention == CXCallingConv_X86_64SysV && abi == c_abi_t::SYSV)) {
            return std::nullopt;
        }

        // FutureBlitz is for 64-bit targets only, where clang reads stdcall and fastcall as C's.
        switch (convention) {
        case CXCallingConv_X86RegCall:
            return "regcall";
        case CXCallingConv_X86VectorCall:
            return "vectorcall";
        case CXCallingConv_Win64:
            return "ms_abi";
        case CXCallingConv_X86_64SysV:
            return "sysv_abi";
        default:
            return "";
        }
    }

    /// "a function of the vectorcall calling convention, not C's".
    static std::string described(const std::string& convention, const std::string_view what) {
        return convention.empty()
                   ? std::format("a {} of a calling convention other than C's", what)
                   : std::format("a {} of the {} calling convention, not C's", what, convention);
    }

    mapped_t map_callback(const CXType function) {
        if (function.kind == CXType_FunctionNoProto) {
            return std::unexpected("a callback that doesn't say what it takes");
        }

        if (clang_isFunctionTypeVariadic(function) != 0) {
            return std::unexpected("a callback that takes varargs");
        }

        if (const std::optional<std::string> convention = convention_of(function)) {
            return std::unexpected(described(*convention, "callback"));
        }

        std::vector<std::string> parameters;
        const int count = std::max(clang_getNumArgTypes(function), 0);
        for (int i = 0; i < count; ++i) {
            const CXType written = clang_getArgType(function, static_cast<unsigned>(i));
            const CXType parameter = clang_getCanonicalType(written);
            if (parameter.kind == CXType_Record) {
                return std::unexpected("a callback that takes a struct by value");
            }

            const mapped_t mapped = map_parameter(written, parameter);
            if (!mapped) {
                return std::unexpected(std::format("a callback that takes {}", mapped.error()));
            }

            parameters.push_back(*mapped);
        }

        std::string text = "Callback(";
        for (std::size_t i = 0; i < parameters.size(); ++i) {
            text += (i > 0 ? ", " : "") + parameters[i];
        }

        text += ')';
        const CXType written = clang_getResultType(function);
        const CXType result = clang_getCanonicalType(written);
        if (result.kind == CXType_Record) {
            return std::unexpected("a callback that gives a struct by value");
        }

        if (result.kind != CXType_Void) {
            const mapped_t mapped = map_type(written);
            if (!mapped) {
                return std::unexpected(std::format("a callback that gives {}", mapped.error()));
            }

            text += ": " + *mapped;
        }

        return text;
    }

    mapped_t map_record(const CXType type) {
        const CXCursor declaration = clang_getTypeDeclaration(type);
        if (clang_getCursorKind(declaration) == CXCursor_UnionDecl) {
            return std::unexpected("a union");
        }

        record_t* record = record_of(type);
        if (record == nullptr) {
            const std::string name = text_of(clang_getTypeSpelling(type));
            return std::unexpected(clang_Cursor_isNull(clang_getCursorDefinition(declaration)) != 0
                                       ? std::format("{}, which no header defines", name)
                                       : std::format("{}, which another header defines", name));
        }

        if (!usable(*record)) {
            return std::unexpected(
                record->c_name.empty()
                    ? std::string("a struct without a name")
                    : std::format("struct {}, which is skipped", record->c_name));
        }

        return record->name;
    }

    mapped_t map_enum(const CXType type) {
        const CXCursor declaration = clang_getTypeDeclaration(type);
        if (const auto found = m_enums.find(usr_of(declaration));
            found != m_enums.end() && !found->second.constants) {
            return found->second.name;
        }

        return integer_type(clang_getEnumDeclIntegerType(declaration));
    }

    /// C's `int16_t m[2][3]` is an Int16[2, 3], laid out the same way.
    mapped_t map_array(CXType type) {
        std::vector<long long> sizes;
        while (clang_getCanonicalType(type).kind == CXType_ConstantArray) {
            type = written_as(type, CXType_ConstantArray);
            sizes.push_back(clang_getArraySize(type));
            type = clang_getArrayElementType(type);
        }

        if (std::ranges::contains(sizes, 0)) {
            return std::unexpected("an array without elements");
        }

        const mapped_t element = map_type(type);
        if (!element) {
            return std::unexpected(std::format("an array of {}", element.error()));
        }

        // A Callback that gives something goes in parentheses, as `(Callback(CLong): CLong)[3]`,
        // since without them the array would be what it gives.
        const bool gives = element->starts_with("Callback(") && element->contains("): ");
        std::string text = (gives ? "(" + *element + ")" : *element) + "[";
        for (std::size_t i = 0; i < sizes.size(); ++i) {
            text += std::format("{}{}", i > 0 ? ", " : "", sizes[i]);
        }

        return text + "]";
    }

    /// Why a struct can't be a @CLayout Struct: it has no name, no fields, a field without a
    /// FutureBlitz type, or a layout other than its fields' types give, as a packed one has.
    std::optional<std::string> problem_of(const record_t& record) {
        if (record.c_name.empty()) {
            return "it has no name";
        }

        std::optional<std::string> problem;
        const std::string_view moved = "it's packed or aligned unlike its fields";
        std::uint64_t offset = 0;
        std::uint64_t align = 1;
        std::size_t fields = 0;
        each_child(record.definition, [&](const CXCursor field) {
            // A union or struct inside without a name puts its fields in this one's.
            const CXCursorKind kind = clang_getCursorKind(field);
            if (!problem && (kind == CXCursor_UnionDecl || kind == CXCursor_StructDecl) &&
                clang_Cursor_isAnonymousRecordDecl(field) != 0) {
                problem = "it holds a union or struct without a name";
            }

            if (problem || kind != CXCursor_FieldDecl) {
                return;
            }

            ++fields;
            const std::string name = spelling_of(field);
            const CXType type = clang_getCanonicalType(clang_getCursorType(field));
            if (clang_Cursor_isBitField(field) != 0) {
                problem = std::format("its field {} is a bit field", name);
                return;
            }

            if (name.empty()) {
                problem = "it has a field without a name";
                return;
            }

            // The type as written, as emit_struct() writes it.
            if (const mapped_t mapped = map_type(clang_getCursorType(field)); !mapped) {
                problem = std::format("its field {} is {}", name, mapped.error());
                return;
            }

            const auto size = static_cast<std::uint64_t>(clang_Type_getSizeOf(type));
            const auto field_align = static_cast<std::uint64_t>(clang_Type_getAlignOf(type));
            offset = (offset + field_align - 1) / field_align * field_align;
            if (clang_Cursor_getOffsetOfField(field) != static_cast<long long>(offset * 8)) {
                problem = moved;
                return;
            }

            offset += size;
            align = std::max(align, field_align);
        });
        if (problem) {
            return problem;
        }

        if (fields == 0) {
            return "it has no fields";
        }

        const std::uint64_t size = (offset + align - 1) / align * align;
        const CXType type = clang_getCursorType(record.definition);
        if (clang_Type_getSizeOf(type) != static_cast<long long>(size) ||
            clang_Type_getAlignOf(type) != static_cast<long long>(align)) {
            return std::string(moved);
        }

        return std::nullopt;
    }

    // Declarations

    void skip(const CXCursor cursor, const std::string& what, const bool in_extern = false) {
        m_warnings.push_back(std::format("{}: skipped {}", where(cursor), what));
        const std::string indent = in_extern ? "    " : "";
        const std::vector<std::string> lines = comment_lines(indent, "skipped: " + what);
        if (in_extern) {
            m_externs.insert(m_externs.end(), lines.begin(), lines.end());
        } else {
            m_blocks.push_back({ .lines = lines, .single = true });
        }
    }

    /// A comment in lines of 100 columns at most, broken between words.
    static std::vector<std::string> comment_lines(const std::string& indent,
                                                  const std::string& text) {
        constexpr std::size_t width = 100;
        std::vector<std::string> lines;
        std::string line = indent + "//";
        std::size_t start = 0;
        while (start < text.size()) {
            const std::size_t end = std::min(text.find(' ', start), text.size());
            const std::string word = text.substr(start, end - start);
            if (line.size() + 1 + word.size() > width && line.size() > indent.size() + 2) {
                lines.push_back(line);
                line = indent + "//";
            }

            line += " " + word;
            start = end + 1;
        }

        lines.push_back(line);
        return lines;
    }

    void emit_item(const CXCursor item) {
        switch (clang_getCursorKind(item)) {
        case CXCursor_StructDecl:
        case CXCursor_UnionDecl:
        case CXCursor_EnumDecl:
            if (is_definition(item)) {
                emit_definition(item, false);
            }

            break;
        case CXCursor_FunctionDecl:
            emit_function(item);
            break;
        case CXCursor_VarDecl:
            skip(item, std::format("{}, a C global", spelling_of(item)));
            break;
        case CXCursor_MacroDefinition:
            emit_macro(item);
            break;
        default:
            break;
        }
    }

    /// A struct, union, or enum, after those it defines inside it, which C names as if they
    /// were outside it. One inside another that has no name says nothing: what holds it is
    /// skipped and says why.
    void emit_definition(const CXCursor definition, const bool nested) {
        const std::string usr = usr_of(definition);
        if (!m_emitted.insert(usr).second) {
            return;
        }

        each_child(definition, [this](const CXCursor child) {
            if (is_definition(child)) {
                emit_definition(child, true);
            }
        });
        switch (clang_getCursorKind(definition)) {
        case CXCursor_StructDecl:
            emit_struct(m_records.at(usr), nested);
            break;
        case CXCursor_UnionDecl: {
            const std::string name = c_name_of(definition);
            if (!name.empty()) {
                skip(definition, std::format("union {}, a union, which FutureBlitz hasn't", name));
            } else if (!nested) {
                skip(definition, "a union, which FutureBlitz hasn't");
            }

            break;
        }
        default:
            emit_enum(definition);
            break;
        }
    }

    void emit_struct(record_t& record, const bool nested) {
        if (!usable(record)) {
            if (!record.c_name.empty() || !nested) {
                const std::string name = record.c_name.empty()
                                             ? std::string("a struct")
                                             : std::format("struct {}", record.c_name);
                skip(record.definition,
                     std::format("{}, since {}", name, record.problem.value_or("")));
            }

            return;
        }

        // The structs it holds come first.
        std::vector<CXCursor> fields;
        std::vector<std::string> c_names;
        each_child(record.definition, [&](const CXCursor field) {
            if (clang_getCursorKind(field) != CXCursor_FieldDecl) {
                return;
            }

            CXType type = clang_getCanonicalType(clang_getCursorType(field));
            while (type.kind == CXType_ConstantArray) {
                type = clang_getCanonicalType(clang_getArrayElementType(type));
            }

            if (const record_t* inner = type.kind == CXType_Record ? record_of(type) : nullptr) {
                emit_definition(inner->definition, false);
            }

            fields.push_back(field);
            c_names.push_back(spelling_of(field));
        });
        block_t block;
        block.lines.push_back(std::format("@CLayout Public Struct {}", record.name));
        const std::vector<std::string> names = namer_t().name(c_names);
        for (std::size_t i = 0; i < fields.size(); ++i) {
            // usable() mapped each field's type already, so each maps.
            const mapped_t mapped = map_type(clang_getCursorType(fields[i]));
            block.lines.push_back(wrapped(
                std::format("    Public Field {}: {}", names[i], mapped.value_or("Ptr<Void>"))));
        }

        block.lines.emplace_back("End Struct");
        m_blocks.push_back(std::move(block));
    }

    void emit_enum(const CXCursor item) {
        const enumeration_t& enumeration = m_enums.at(usr_of(item));
        std::vector<CXCursor> constants;
        each_constant(item, [&](const CXCursor constant) { constants.push_back(constant); });
        if (constants.empty()) {
            return;
        }

        // A constant's value as a type has it.
        const auto value_of = [](const CXCursor constant, const std::string& type) {
            const std::uint64_t bits =
                type.starts_with('U')
                    ? clang_getEnumConstantDeclUnsignedValue(constant)
                    : static_cast<std::uint64_t>(clang_getEnumConstantDeclValue(constant));
            return integer_literal(type, bits);
        };

        // An enum's constants are Consts of its integer type, which the functions that take it
        // take, or of C's type for each when nothing names the enum's type.
        if (enumeration.constants) {
            for (const CXCursor constant : constants) {
                const std::string type =
                    enumeration.anonymous
                        ? integer_type(clang_getCursorType(constant)).value_or(enumeration.integer)
                        : enumeration.integer;
                const std::string line =
                    std::format("Public Const {}: {} = {}", m_constant_names.at(usr_of(constant)),
                                type, value_of(constant, type));
                m_blocks.push_back({ .lines = { line }, .single = true });
            }

            return;
        }

        block_t block;
        block.lines.push_back(
            std::format("Public Enum {}: {}", enumeration.name, enumeration.integer));
        std::vector<std::string> c_names;
        c_names.reserve(constants.size());
        for (const CXCursor constant : constants) {
            c_names.push_back(spelling_of(constant));
        }

        const std::vector<std::string> names = namer_t().name(c_names);
        for (std::size_t i = 0; i < constants.size(); ++i) {
            block.lines.push_back(
                std::format("    {} = {}", names[i], value_of(constants[i], enumeration.integer)));
        }

        block.lines.emplace_back("End Enum");
        m_blocks.push_back(std::move(block));
    }

    void emit_function(const CXCursor item) {
        const std::string c_name = spelling_of(item);
        const function_t& function = m_functions.at(c_name);
        // A function declared more than once is bound once, where it's first declared.
        if (clang_equalCursors(function.first, item) == 0) {
            return;
        }

        const CXCursor declaration = function.declaration;
        // The canonical type's parameters are as C passes them, an array as a pointer, where
        // the declared type's are as written, with the names that say which integers are C's.
        const CXType type = clang_getCanonicalType(clang_getCursorType(declaration));
        const CXType declared = written_as(clang_getCursorType(declaration), CXType_FunctionProto);
        if (clang_Cursor_getStorageClass(item) == CX_SC_Static ||
            clang_Cursor_getStorageClass(declaration) == CX_SC_Static) {
            skip(item, std::format("{}, a static function, which no library has", c_name), true);
            return;
        }

        if (clang_Cursor_isFunctionInlined(item) != 0 ||
            clang_Cursor_isFunctionInlined(declaration) != 0) {
            skip(item, std::format("{}, an inline function, which no library has", c_name), true);
            return;
        }

        if (type.kind == CXType_FunctionNoProto) {
            skip(item, std::format("{}, a function that doesn't say what it takes", c_name), true);
            return;
        }

        if (clang_isFunctionTypeVariadic(type) != 0) {
            skip(item,
                 std::format("{}, a varargs function, which FutureBlitz can't call; wrap it in C",
                             c_name),
                 true);
            return;
        }

        if (const std::optional<std::string> convention = convention_of(type)) {
            skip(item, std::format("{}, {}", c_name, described(*convention, "function")), true);
            return;
        }

        if (c_name == "main" || c_name.starts_with("fb_")) {
            skip(item, std::format("{}, a symbol the program defines itself", c_name), true);
            return;
        }

        // A target whose C ABI FutureBlitz doesn't have passes no Struct by value.
        if (by_value_struct(type) && c_abi_of(target()) == c_abi_t::OTHER) {
            skip(item,
                 std::format("{}, which passes a struct by value, which FutureBlitz does only on "
                             "x86-64 and AArch64",
                             c_name),
                 true);
            return;
        }

        std::vector<std::string> types;
        std::vector<std::string> c_names;
        const int count = std::max(clang_getNumArgTypes(type), 0);
        for (int i = 0; i < count; ++i) {
            const mapped_t mapped =
                map_parameter(clang_getArgType(declared, static_cast<unsigned>(i)),
                              clang_getArgType(type, static_cast<unsigned>(i)));
            if (!mapped) {
                skip(item, std::format("{}, which takes {}", c_name, mapped.error()), true);
                return;
            }

            types.push_back(*mapped);
            c_names.push_back(parameter_name(function, static_cast<unsigned>(i)));
        }

        const std::vector<std::string> names = namer_t().name(c_names, "arg");
        std::string line = std::format("    Function {}(", function.name);
        for (std::size_t i = 0; i < types.size(); ++i) {
            line += std::format("{}{}: {}", i > 0 ? ", " : "", names[i], types[i]);
        }

        line += ')';
        const CXType written = clang_getResultType(declared);
        const CXType result = clang_getCanonicalType(written);
        if (result.kind != CXType_Void) {
            const mapped_t mapped = map_type(written);
            if (!mapped) {
                skip(item, std::format("{}, which gives {}", c_name, mapped.error()), true);
                return;
            }

            line += ": " + *mapped;
        }

        if (function.name != c_name) {
            line += std::format(" = \"{}\"", c_name);
        }

        m_externs.push_back(wrapped(line));
    }

    /// A parameter's name: the bound declaration's, or else the last other declaration's that
    /// names it, as C lets each declaration name its own.
    static std::string parameter_name(const function_t& function, const unsigned at) {
        std::string name = spelling_of(clang_Cursor_getArgument(function.declaration, at));
        for (const CXCursor declaration : function.declarations | std::views::reverse) {
            if (name.empty() && std::cmp_less(at, clang_Cursor_getNumArguments(declaration))) {
                name = spelling_of(clang_Cursor_getArgument(declaration, at));
            }
        }

        return name;
    }

    /// The target the module is for.
    [[nodiscard]] std::string target() const {
        return m_options.target.empty() ? std::string(build_config::target_triple)
                                        : m_options.target;
    }

    /// Whether a canonical function type takes or gives a struct by value.
    static bool by_value_struct(const CXType function) {
        const auto is_record = [](const CXType type) {
            return clang_getCanonicalType(type).kind == CXType_Record;
        };
        const int count = std::max(clang_getNumArgTypes(function), 0);
        for (int i = 0; i < count; ++i) {
            if (is_record(clang_getArgType(function, static_cast<unsigned>(i)))) {
                return true;
            }
        }

        return is_record(clang_getResultType(function));
    }

    /// A line broken to stay within 100 columns where it can, as place() breaks it.
    static std::string wrapped(const std::string& line) {
        std::string text;
        std::size_t column = 0;
        place(line, line.find_first_not_of(' '), column, text);
        return text;
    }

    /// Writes `text` at `column`, broken where it doesn't fit within 100 columns: inside its
    /// first parentheses that hold anything, after the commas between what they hold, the lines
    /// lined up after the parenthesis, or a step in from `indent` where the longest part doesn't
    /// fit there. A part that doesn't fit on a line of its own is broken the same way.
    static void place(const std::string_view text, const std::size_t indent, std::size_t& column,
                      std::string& out) {
        constexpr std::size_t width = 100;
        std::size_t open = text.find('(');
        while (open != std::string_view::npos && open + 1 < text.size() && text[open + 1] == ')') {
            open = text.find('(', open + 1);
        }

        // The parts, each with its comma, and the last with what follows the parentheses.
        std::vector<std::string_view> parts;
        std::size_t start = open + 1;
        std::size_t depth = 0;
        bool closed = false;
        for (std::size_t i = start; open != std::string_view::npos && !closed && i < text.size();
             ++i) {
            if (text[i] == '(') {
                ++depth;
            } else if (text[i] == ')' && depth == 0) {
                closed = true;
            } else if (text[i] == ')') {
                --depth;
            } else if (text[i] == ',' && depth == 0 && i + 1 < text.size() && text[i + 1] == ' ') {
                parts.push_back(text.substr(start, i + 1 - start));
                start = i + 2;
            }
        }

        if (column + text.size() <= width || !closed) {
            out += text;
            column += text.size();
            return;
        }

        parts.push_back(text.substr(start));
        out += text.substr(0, open + 1);
        column += open + 1;
        const std::size_t longest = std::ranges::max(
            parts | std::views::transform([](const std::string_view part) { return part.size(); }));
        std::size_t at = column;
        if (column + longest > width && indent + 4 < column) {
            at = indent + 4;
            out += "\n" + std::string(at, ' ');
            column = at;
        }

        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (i > 0 && column + 1 + parts[i].size() <= width) {
                out += ' ';
                ++column;
            } else if (i > 0) {
                out += "\n" + std::string(at, ' ');
                column = at;
            }

            place(parts[i], at, column, out);
        }
    }

    void emit_macro(const CXCursor item) {
        if (clang_Cursor_isMacroBuiltin(item) != 0) {
            return;
        }

        const macro_t& macro = m_macros.at(spelling_of(item));
        // What C sees after the header is its last definition.
        if (clang_equalCursors(macro.last, item) == 0) {
            return;
        }

        if (!macro.value) {
            if (!macro.value.error().empty()) {
                skip(item, macro.value.error());
            }

            return;
        }

        m_blocks.push_back({
            .lines = {
                std::format("Public Const {}: {} = {}", macro.name, macro.value->type,
                            macro.value->value),
            },
            .single = true,
        });
    }

    // The module

    std::string assemble() {
        const std::string header = to_utf8(m_options.header.filename());
        std::string text = std::format(
            "// Generated by fb-bindgen from {} for {}.\n// Run fb-bindgen again instead of "
            "editing it.\n",
            header, target());
        if (!m_externs.empty()) {
            block_t block;
            block.lines.push_back(
                m_options.library.empty()
                    ? std::string(R"(Public Extern "C")")
                    : std::format(R"(Public Extern "C" Lib "{}")", m_options.library));
            block.lines.insert(block.lines.end(), m_externs.begin(), m_externs.end());
            block.lines.emplace_back("End Extern");
            m_blocks.push_back(std::move(block));
        }

        for (std::size_t i = 0; i < m_blocks.size(); ++i) {
            const bool together = i > 0 && m_blocks[i].single && m_blocks[i - 1].single;
            text += together ? "" : "\n";
            for (const std::string& line : m_blocks[i].lines) {
                text += line + "\n";
            }
        }

        return text;
    }
};

/// The canonical parts of a triple that say which C library it has: its architecture, system,
/// and environment, without its vendor. GCC's "x86_64-w64-mingw32" is "x86_64-w64-windows-gnu".
std::vector<std::string> target_parts(const std::string_view triple) {
    char* normal = LLVMNormalizeTargetTriple(std::string(triple).c_str());
    const std::string text = normal != nullptr ? normal : "";
    LLVMDisposeMessage(normal);
    std::vector<std::string> parts;
    for (const auto part : std::views::split(text, '-')) {
        parts.emplace_back(part.begin(), part.end());
    }

    parts.resize(4);
    parts.erase(parts.begin() + 1);
    if (parts[0] == "amd64") {
        parts[0] = "x86_64";
    }

    return parts;
}

/// Whether clang reads C with `arguments`, in an empty file, which says whether they're what
/// keeps it from reading a header.
bool clang_takes(CXIndex index, const std::vector<std::string>& arguments) {
    std::vector<const char*> argv;
    argv.reserve(arguments.size());
    for (const std::string& argument : arguments) {
        argv.push_back(argument.c_str());
    }

    CXUnsavedFile empty{ .Filename = "fb-bindgen-empty.c", .Contents = "", .Length = 0 };
    CXTranslationUnit parsed = nullptr;
    const CXErrorCode code = clang_parseTranslationUnit2(
        index, empty.Filename, argv.data(), static_cast<int>(argv.size()), &empty, 1, 0, &parsed);
    const unit_t unit(parsed);
    return code == CXError_Success && unit;
}

/// Why libclang read nothing: the target or C standard it was given, which it doesn't know, or
/// else the header.
std::string unreadable(CXIndex index, const std::string& path, const std::string& target,
                       const std::string& standard) {
    const std::vector<std::string> plain{ "-x", "c", "-nostdlibinc" };
    std::vector<std::string> with_target = plain;
    with_target.push_back("--target=" + target);
    std::vector<std::string> with_standard = plain;
    with_standard.push_back("-std=" + standard);
    if (!clang_takes(index, with_target)) {
        return std::format("libclang doesn't know the target '{}'", target);
    }

    if (!clang_takes(index, with_standard)) {
        return std::format("libclang doesn't know the C standard '{}'", standard);
    }

    return std::format("libclang couldn't read {}", path);
}

/// Each file clang read for `unit`: the header and what it #includes, directly or not.
std::vector<std::filesystem::path> files_read(CXTranslationUnit unit) {
    std::vector<std::filesystem::path> files;
    clang_getInclusions(
        unit,
        [](CXFile file, CXSourceLocation* /*stack*/, unsigned /*depth*/, CXClientData data) {
            static_cast<std::vector<std::filesystem::path>*>(data)->push_back(
                path_from_utf8(text_of(clang_getFileName(file))));
        },
        &files);
    return files;
}

/// The errors clang found reading the header, a line each: "test.h:2:10: 'nowhere.h' file not
/// found".
std::string errors_of(CXTranslationUnit unit) {
    std::string errors;
    each_diagnostic(unit, [&](CXDiagnostic diagnostic) {
        if (clang_getDiagnosticSeverity(diagnostic) < CXDiagnostic_Error) {
            return;
        }

        CXFile file = nullptr;
        unsigned line = 0;
        unsigned column = 0;
        clang_getSpellingLocation(clang_getDiagnosticLocation(diagnostic), &file, &line, &column,
                                  nullptr);
        const std::string message = text_of(clang_getDiagnosticSpelling(diagnostic));
        errors += errors.empty() ? "" : "\n";
        errors += file != nullptr ? std::format("{}:{}:{}: {}", text_of(clang_getFileName(file)),
                                                line, column, message)
                                  : message;
    });
    return errors;
}

/// The folders of this target's C headers: the toolchain's, or on macOS, where a release has
/// none, the SDK's, from Apple's Command Line Tools.
std::vector<std::string> c_header_folders() {
    std::vector<std::string> folders;
    for (const std::filesystem::path& folder : toolchain().c_headers) {
        folders.push_back(to_utf8(folder));
    }

#ifdef __APPLE__
    if (folders.empty()) {
        const std::vector<std::string> arguments{ "--sdk", "macosx", "--show-sdk-path" };
        const std::expected<process_result_t, std::string> sdk =
            run_process("/usr/bin/xcrun", arguments);
        if (sdk && sdk->exit_code == 0) {
            std::string path = sdk->output;
            while (!path.empty() && (path.back() == '\n' || path.back() == '\r')) {
                path.pop_back();
            }

            if (!path.empty()) {
                folders.push_back(path + "/usr/include");
            }
        }
    }
#endif
    return folders;
}

} // namespace

std::expected<bindings_t, std::string> generate_bindings(const bindgen_options_t& options) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(options.header, error)) {
        return std::unexpected(std::format("{} isn't a file", to_utf8(options.header)));
    }

    // The header is read as the C compiler that links programs reads it: its C standard, with
    // GNU's extensions, its version as GCC's, and its C library's include folders, after clang's
    // own headers, like stddef.h and the intrinsics, which take the place of that compiler's.
    const std::string build_target(build_config::target_triple);
    const std::string target = options.target.empty() ? build_target : options.target;
    const bool foreign = target_parts(target) != target_parts(build_target);
    const std::string standard =
        options.standard.empty() ? std::string(bindgen_config::c_standard) : options.standard;
    std::vector<std::string> arguments{
        "-x",
        "c",
        "-std=" + standard,
        "--target=" + target,
        "-nostdlibinc",
        "-resource-dir",
        to_utf8(toolchain().clang_headers),
    };
    for (const std::string& folder : options.includes) {
        arguments.push_back("-I" + folder);
    }

    // Another target's C library isn't this one's: its folders come through -I, and clang's
    // own headers, like stdint.h, stand on their own.
    std::vector<std::string> system = options.system_includes;
    if (system.empty() && !foreign) {
        system = c_header_folders();
        if (!bindgen_config::gnuc_version.empty()) {
            arguments.push_back(std::format("-fgnuc-version={}", bindgen_config::gnuc_version));
        }
    } else if (system.empty()) {
        arguments.emplace_back("-ffreestanding");
    }

    for (const std::string& folder : system) {
        arguments.emplace_back("-idirafter");
        arguments.push_back(folder);
    }

    for (const std::string& define : options.defines) {
        arguments.push_back("-D" + define);
    }

    std::vector<const char*> argv;
    argv.reserve(arguments.size());
    for (const std::string& argument : arguments) {
        argv.push_back(argument.c_str());
    }

    const std::unique_ptr<void, index_deleter_t> index(clang_createIndex(0, 0));
    CXTranslationUnit parsed = nullptr;
    const std::string path = to_utf8(options.header);
    const CXErrorCode code = clang_parseTranslationUnit2(
        index.get(), path.c_str(), argv.data(), static_cast<int>(argv.size()), nullptr, 0,
        static_cast<unsigned>(CXTranslationUnit_DetailedPreprocessingRecord) |
            static_cast<unsigned>(CXTranslationUnit_SkipFunctionBodies),
        &parsed);
    const unit_t unit(parsed);
    if (code != CXError_Success || !unit) {
        return std::unexpected(unreadable(index.get(), path, target, standard));
    }

    if (std::string errors = errors_of(unit.get()); !errors.empty()) {
        return std::unexpected(std::move(errors));
    }

    std::expected<bindings_t, std::string> bindings =
        generator_t(options, index.get(), arguments, unit.get()).run();
    if (bindings) {
        bindings->files = files_read(unit.get());
    }

    return bindings;
}

} // namespace fb
