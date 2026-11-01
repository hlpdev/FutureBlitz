#include "fb/mcp.hpp"

#include "fb/build_config.hpp"
#include "fb/diagnostic.hpp"
#include "fb/diagnostic_render.hpp"
#include "fb/doc.hpp"
#include "fb/driver.hpp"
#include "fb/format.hpp"
#include "fb/sema.hpp"
#include "fb/source.hpp"
#include "fb/spec_pages.hpp"
#include "fb/utf8.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace fb {

namespace {

using json_t = nlohmann::json;

/// MCP's versions this server speaks, the newest first, which it answers an initialize with when
/// the client's isn't one of them.
constexpr std::array<std::string_view, 3> protocol_versions{
    "2025-06-18",
    "2025-03-26",
    "2024-11-05",
};

/// JSON-RPC's error codes.
constexpr int parse_error = -32700;
constexpr int invalid_request = -32600;
constexpr int method_not_found = -32601;
constexpr int invalid_params = -32602;

/// How much of one spec section or one std declaration's doc a search gives back.
constexpr std::size_t excerpt_length = 1800;

/// Something a search can find: a section of the spec, or one of std's declarations.
struct doc_entry_t {
    /// "Control flow > For loops", or a declaration's path, like "std.text.split.Split".
    std::string title;
    /// Where it is: "docs/spec/04-control-flow.md", or "std/text/split.fb:12".
    std::string where;
    /// The section's Markdown, or the declaration's first line and its doc.
    std::string text;
    /// The title and text in lower case, which a search's words are found in.
    std::string lower_title;
    std::string lower_text;
};

std::string lowered(const std::string_view text) {
    std::string lower(text);
    std::ranges::transform(lower, lower.begin(), [](const unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return lower;
}

doc_entry_t entry_of(std::string title, std::string where, std::string text) {
    doc_entry_t entry{
        .title = std::move(title),
        .where = std::move(where),
        .text = std::move(text),
    };
    entry.lower_title = lowered(entry.title);
    entry.lower_text = lowered(entry.text);
    return entry;
}

/// The spec's sections, one for each heading of each page, titled by the page's heading and
/// its own.
std::vector<doc_entry_t> spec_entries() {
    std::vector<doc_entry_t> entries;
    for (const spec_page_t& page : spec_pages) {
        std::string page_title;
        std::string heading;
        std::string body;
        const auto flush = [&] {
            if (!heading.empty() || !body.empty()) {
                const std::string title = heading.empty() || heading == page_title
                                              ? page_title
                                              : std::format("{} > {}", page_title, heading);
                entries.push_back(entry_of(title, std::format("docs/spec/{}", page.file), body));
            }

            body.clear();
        };
        std::string whole;
        for (const std::string_view piece : page.text) {
            whole += piece;
        }

        std::istringstream lines{ whole };
        std::string line;
        bool fenced = false;
        while (std::getline(lines, line)) {
            if (line.ends_with('\r')) {
                line.pop_back();
            }

            if (line.starts_with("```")) {
                fenced = !fenced;
            }

            if (!fenced && line.starts_with('#')) {
                flush();
                heading = line.substr(line.find_first_not_of('#'));
                heading.erase(0, heading.find_first_not_of(' '));
                if (page_title.empty()) {
                    page_title = heading;
                }

                continue;
            }

            body += line;
            body += '\n';
        }

        flush();
    }

    return entries;
}

/// A declaration of std's reference, and each of its members, as entries.
void add_items(const std::vector<doc_item_t>& items, std::vector<doc_entry_t>& entries) {
    for (const doc_item_t& item : items) {
        std::string text = item.signature;
        if (item.doc) {
            text += "\n\n";
            text += item.doc->markdown;
        }

        entries.push_back(
            entry_of(item.path, std::format("std/{}:{}", item.file, item.line), text));
        add_items(item.members, entries);
    }
}

bool is_word_char(const char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

/// How often `word` is in `text`, and how often as a word of its own, not inside another.
struct occurrences_t {
    std::size_t inside = 0;
    std::size_t whole = 0;
};

occurrences_t occurrences(const std::string_view text, const std::string_view word) {
    occurrences_t found;
    for (std::size_t at = text.find(word); at != std::string_view::npos;
         at = text.find(word, at + word.size())) {
        ++found.inside;
        const std::size_t end = at + word.size();
        if ((at == 0 || !is_word_char(text[at - 1])) &&
            (end == text.size() || !is_word_char(text[end]))) {
            ++found.whole;
        }
    }

    return found;
}

/// `text` cut to at most `length` bytes, at its last line break before them if it has one there,
/// and never inside a UTF-8 sequence.
std::string excerpt_of(const std::string_view text, const std::size_t length) {
    if (text.size() <= length) {
        return std::string(text);
    }

    std::size_t end = text.rfind('\n', length);
    if (end == std::string_view::npos || end < length / 2) {
        end = length;
        while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U) {
            --end;
        }
    }

    return std::format("{}\n...", text.substr(0, end));
}

/// Words a search leaves out, which most of the docs have, each between spaces.
constexpr std::string_view filler_words = " a an and the to of in is it how do i can on or ";

/// A search's words: its letters, digits, and '_'s, in lower case, without its filler.
std::vector<std::string> words_of(const std::string_view query) {
    std::vector<std::string> words;
    std::string word;
    const auto add = [&] {
        if (!word.empty() && !filler_words.contains(std::format(" {} ", word))) {
            words.push_back(word);
        }

        word.clear();
    };
    for (const char c : query) {
        if (is_word_char(c)) {
            word += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else {
            add();
        }
    }

    add();
    return words;
}

/// The last part of an entry's title, a declaration's name or a section's heading.
std::string_view last_part(const std::string_view title) {
    const std::size_t dot = title.rfind('.');
    const std::size_t heading = title.rfind("> ");
    if (heading != std::string_view::npos && (dot == std::string_view::npos || heading > dot)) {
        return title.substr(heading + 2);
    }

    return dot == std::string_view::npos ? title : title.substr(dot + 1);
}

/// The text of the first line of a span, without its indentation.
std::string first_line(const source_manager_t& sources, const span_t span) {
    const source_file_t& file = sources.get(span.file);
    const std::string_view text = file.text();
    const std::size_t start = text.rfind('\n', span.begin == 0 ? 0 : span.begin - 1);
    const std::size_t begin = start == std::string_view::npos || span.begin == 0 ? 0 : start + 1;
    const std::size_t end = std::min(text.find('\n', span.begin), text.size());
    std::string line(text.substr(begin, end - begin));
    line.erase(0, line.find_first_not_of(" \t"));
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
        line.pop_back();
    }

    return line;
}

/// A tool's result: its text, and whether it's an error.
struct tool_result_t {
    std::string text;
    bool error = false;
};

json_t text_result(const tool_result_t& result) {
    return json_t{
        { "content", json_t::array({ json_t{ { "type", "text" }, { "text", result.text } } }) },
        { "isError", result.error },
    };
}

/// A tool's description, for tools/list.
json_t tool(const std::string_view name, const std::string_view description, json_t properties,
            json_t required) {
    return json_t{
        { "name", name },
        { "description", description },
        {
            "inputSchema",
            json_t{
                { "type", "object" },
                { "properties", std::move(properties) },
                { "required", std::move(required) },
            },
        },
    };
}

json_t tools() {
    return json_t::array({
        tool("search_docs",
             "Searches FutureBlitz's language spec and its standard library's reference for the "
             "words given, like 'For loop', 'string split', or 'Map', and gives the sections and "
             "declarations that have them, best first.",
             json_t{
                 {
                     "query",
                     json_t{ { "type", "string" }, { "description", "Words to search for" } },
                 },
                 {
                     "limit",
                     json_t{
                         { "type", "integer" },
                         { "description", "How many results, 6 unless it says" },
                     },
                 },
             },
             json_t::array({ "query" })),
        tool("lookup",
             "Looks up a name in the package and the standard library, like 'Split', "
             "'std.collections.map.Map', or one of the package's own Functions, and gives each "
             "declaration of it: its first line, where it is, and its doc. It runs none of the "
             "package's build steps: their files are what fb build last made.",
             json_t{
                 {
                     "name",
                     json_t{
                         { "type", "string" },
                         { "description", "The name, or a dotted path" },
                     },
                 },
             },
             json_t::array({ "name" })),
        tool("check",
             "Checks FutureBlitz code for errors without building or running it, and gives each "
             "error and warning with its line, its fix when it has one, and its notes. With 'code' "
             "alone, it checks that as a program of its own; with 'file', a .fb file in the "
             "package's folder, with 'code' as its text if it's given, though it isn't saved; with "
             "neither, the whole package, or each .fb file of a folder that isn't one. It runs "
             "none of the package's build steps: their files are what fb build last made, and one "
             "that hasn't run is an error that says so.",
             json_t{
                 { "code", json_t{ { "type", "string" }, { "description", "FutureBlitz code" } } },
                 {
                     "file",
                     json_t{
                         { "type", "string" },
                         { "description", "A .fb file in the package's folder, from it" },
                     },
                 },
             },
             json_t::array()),
        tool("format",
             "Formats FutureBlitz code as fb fmt does, and gives it back, or the errors that keep "
             "it from being formatted. It writes nothing.",
             json_t{
                 { "code", json_t{ { "type", "string" }, { "description", "FutureBlitz code" } } },
                 {
                     "file",
                     json_t{
                         { "type", "string" },
                         { "description", "A .fb file in the package's folder, from it" },
                     },
                 },
                 {
                     "width",
                     json_t{
                         { "type", "integer" },
                         { "description", "The line width, 100 unless it says" },
                     },
                 },
             },
             json_t::array()),
    });
}

} // namespace

std::string_view agents_primer() {
    return R"md(# FutureBlitz

This is a FutureBlitz package. FutureBlitz is a compiled language in BASIC's family: `.fb` files,
keywords in title case (`Function`, `If`, `End If`), `//` comments, a name's type after it
(`Local count: Int = 0`), and a statement a line.

```fb
Import std.core

Function Square(n: Int): Int => n * n

For i = 1 To 5
    Print("{i} squared is {Square(i)}")
End For
```

- `fb run` builds and runs the package, `fb build` builds it, `fb test` runs its `@Test`s,
  `fb check` gives its errors without building it, and `fb fmt` formats its files.
- Its standard library is `std`: `Import std.core`, `std.collections`, `std.io`, `std.text`, and
  the rest.
- `fb mcp` is a Model Context Protocol server, over stdio, whose tools know FutureBlitz:
  `search_docs` searches the language's spec and std's reference, `lookup` finds a name's
  declarations, `check` checks code or a file and gives its errors, and `format` formats code.
  An assistant runs it as the command `fb mcp`, in the package's folder.
- FutureBlitz isn't Visual Basic or any other language: look a feature up with `search_docs`
  before using it, and `check` code before saying it works.
)md";
}

namespace {

/// What searches and lookups search: the spec's sections and std's declarations, and std's
/// compilation, for its built-in functions.
struct docs_t { // NOLINT(bugprone-exception-escape)
    std::vector<doc_entry_t> entries;
    std::optional<compilation_t> standard;
    /// Why std's reference isn't among the entries, when it isn't.
    std::string error;
};

docs_t load_docs() {
    docs_t docs{ .entries = spec_entries() };
    std::expected<compilation_t, std::string> compiled = compile_standard_docs();
    if (!compiled) {
        docs.error = compiled.error();
        return docs;
    }

    diagnostic_bag_t warnings;
    const doc_package_t package = document(*compiled, true, build_mode_t::DEBUG, warnings);
    for (const doc_module_t& module : package.modules) {
        const std::string text = module.doc ? module.doc->markdown : std::string();
        docs.entries.push_back(entry_of(module.name, std::format("std/{}", module.file), text));
        add_items(module.items, docs.entries);
    }

    docs.standard = std::move(*compiled);
    return docs;
}

} // namespace

namespace {

/// How deep a message's arrays and objects nest at most, which MCP's messages never come near.
constexpr int deepest_nesting = 64;

/// Whether a line's JSON nests its arrays and objects deeper than deepest_nesting, worked out
/// before it's parsed, as copying a value nested that deep takes as much stack.
bool too_deep(const std::string_view line) {
    int depth = 0;
    bool quoted = false;
    for (std::size_t at = 0; at < line.size(); ++at) {
        const char c = line[at];
        if (quoted) {
            if (c == '\\') {
                ++at;
            } else if (c == '"') {
                quoted = false;
            }
        } else if (c == '"') {
            quoted = true;
        } else if (c == '[' || c == '{') {
            if (++depth > deepest_nesting) {
                return true;
            }
        } else if (c == ']' || c == '}') {
            --depth;
        }
    }

    return false;
}

json_t failure(const json_t& id, const int code, const std::string_view message) {
    return {
        { "jsonrpc", "2.0" },
        { "id", id },
        { "error", json_t{ { "code", code }, { "message", message } } },
    };
}

json_t success(const json_t& id, json_t result) {
    return { { "jsonrpc", "2.0" }, { "id", id }, { "result", std::move(result) } };
}

/// What takes a message's answer: the line it's sent as, or its batch's; nullopt for a call
/// that's cancelled, which a batch waits for no more.
using reply_t = std::function<void(const std::optional<json_t>& answer)>;

/// The tools, by name.
constexpr std::array<std::string_view, 4> tool_names{ "search_docs", "lookup", "check", "format" };

/// The errors and warnings of a compilation, as fb check shows them without color, with each
/// fix, and how many of each there are.
void report(const compilation_t& compiled, std::string& text, std::size_t& errors,
            std::size_t& warnings) {
    for (const diagnostic_t& diagnostic : compiled.diagnostics.diagnostics()) {
        (diagnostic.severity == severity_t::ERROR ? errors : warnings) += 1;
        text += render_pretty(diagnostic, compiled.sources);
        for (const suggestion_t& suggestion : diagnostic.suggestions) {
            for (const fix_t& fix : suggestion.fixes) {
                const source_file_t& in = compiled.sources.get(fix.span.file);
                text += std::format("  fix: at line {}, put `{}` in place of `{}`\n",
                                    in.line_of(fix.span.begin) + 1, fix.replacement,
                                    in.text().substr(fix.span.begin, fix.span.length()));
            }
        }

        text += '\n';
    }
}

} // namespace

struct mcp_server_t::core_t : std::enable_shared_from_this<core_t> {
    /// A tool call that's running: what takes its answer, its id, and when it has to be done.
    struct call_t {
        reply_t reply;
        json_t id;
        std::chrono::steady_clock::time_point deadline;
    };

    /// A batch's answers, sent as one once the last is in.
    struct batch_t {
        std::mutex mutex;
        json_t answers = json_t::array();
        /// The answers it waits for, and one for the batch being read.
        std::size_t waiting = 1;
    };

    std::filesystem::path root;
    send_t send;
    mcp_options_t options;
    /// One answer's line at a time.
    std::mutex sending;
    std::mutex mutex;
    std::condition_variable changed;
    /// The calls running, by their ids' JSON.
    std::map<std::string, call_t> calls;
    /// The answers being sent from a call's thread or the deadlines' thread.
    std::size_t replying = 0;
    bool closed = false;
    /// The version the initialize agreed on; only the thread that receives reads it.
    std::string version;
    std::once_flag docs_started;
    std::shared_future<docs_t> loading;
    std::thread deadlines;

    void write(const json_t& message) {
        const std::scoped_lock lock(sending);
        send(message.dump(-1, ' ', false, json_t::error_handler_t::replace));
    }

    void start_docs() {
        std::call_once(docs_started,
                       [this] { loading = std::async(std::launch::async, load_docs).share(); });
    }

    /// The docs, which an initialize starts reading on a thread of their own, as reading std
    /// takes seconds, or the first tool that needs them.
    const docs_t& docs() {
        start_docs();
        return loading.get();
    }

    /// A .fb file a tool names, from the package's folder, which it has to be in.
    [[nodiscard]] std::expected<std::filesystem::path, std::string>
    resolved(const std::string_view given) const {
        std::error_code error;
        std::filesystem::path path =
            std::filesystem::weakly_canonical(root / path_from_utf8(given), error);
        const std::filesystem::path inside =
            path.lexically_relative(std::filesystem::weakly_canonical(root, error));
        if (error || inside.empty() || *inside.begin() == ".." || path.extension() != ".fb") {
            return std::unexpected(std::format(
                "'{}' isn't a .fb file in the package's folder, which is all a tool reads", given));
        }

        return path;
    }

    tool_result_t run(const std::string& tool, const json_t& arguments) {
        if (tool == "search_docs") {
            return search(arguments);
        }

        if (tool == "lookup") {
            return lookup(arguments);
        }

        if (tool == "check") {
            return check(arguments);
        }

        return format(arguments);
    }

    tool_result_t search(const json_t& arguments) {
        const std::string query = arguments.at("query").get<std::string>();
        const auto limit = static_cast<std::size_t>(std::clamp(arguments.value("limit", 6), 1, 20));
        const std::vector<std::string> words = words_of(query);
        if (words.empty()) {
            return { .text = "The query has no words to search for.", .error = true };
        }

        const docs_t& all = docs();
        std::vector<std::pair<std::size_t, const doc_entry_t*>> scored;
        for (const doc_entry_t& entry : all.entries) {
            std::size_t score = 0;
            std::size_t present = 0;
            for (const std::string& word : words) {
                const occurrences_t in_title = occurrences(entry.lower_title, word);
                const occurrences_t in_text = occurrences(entry.lower_text, word);
                score += (in_title.whole * 60) + (in_title.inside * 15) +
                         std::min<std::size_t>(in_text.whole + in_text.inside, 24) +
                         (last_part(entry.lower_title) == word ? 120 : 0);
                present += in_title.inside + in_text.inside > 0 ? 1 : 0;
            }

            // An entry with every word comes before one with some, and a short one a little
            // before a long one with as many.
            if (present > 0) {
                score += present * present * 40;
                scored.emplace_back(score * 1000 / (20 + (entry.text.size() / 1000)), &entry);
            }
        }

        std::ranges::stable_sort(scored, std::ranges::greater{},
                                 &std::pair<std::size_t, const doc_entry_t*>::first);
        if (scored.empty()) {
            return {
                .text = std::format("Nothing in the spec or std's reference has '{}'.", query),
            };
        }

        // Overloads, which share a title, are one result, in the order they're declared.
        std::vector<std::string_view> titles;
        for (const auto& [score, entry] : scored) {
            if (titles.size() < limit && !std::ranges::contains(titles, entry->title)) {
                titles.emplace_back(entry->title);
            }
        }

        std::string text;
        for (const std::string_view title : titles) {
            text += std::format("## {}\n", title);
            for (const doc_entry_t& entry : all.entries) {
                if (entry.title == title) {
                    text += std::format("({})\n\n{}\n\n", entry.where,
                                        excerpt_of(entry.text, excerpt_length));
                }
            }
        }

        if (!all.error.empty()) {
            text += std::format("(std's reference couldn't be read: {})\n", all.error);
        }

        return { .text = text };
    }

    tool_result_t lookup(const json_t& arguments) {
        const std::string name = arguments.at("name").get<std::string>();
        if (name.empty()) {
            return { .text = "The name is empty.", .error = true };
        }

        const std::string last =
            name.substr(name.rfind('.') == std::string::npos ? 0 : name.rfind('.') + 1);
        const auto matches = [&](const std::string_view candidate) {
            return lowered(candidate) == lowered(last);
        };
        std::string text;
        std::size_t found = 0;
        // The package's own declarations, from a check of every file of it, which runs none of
        // its build steps.
        if (find_package(root)) {
            if (std::expected<compilation_t, std::string> compiled =
                    compile_docs(root, build_mode_t::DEBUG, {}, build_steps_t::KEPT)) {
                for (const symbol_t& symbol : compiled->analysis.symbols) {
                    const bool declared = symbol.kind == symbol_kind_t::FUNCTION ||
                                          symbol.kind == symbol_kind_t::GLOBAL ||
                                          symbol.kind == symbol_kind_t::CONSTANT ||
                                          symbol.kind == symbol_kind_t::TYPE ||
                                          symbol.kind == symbol_kind_t::FIELD ||
                                          symbol.kind == symbol_kind_t::METHOD ||
                                          symbol.kind == symbol_kind_t::PROPERTY;
                    if (!declared || symbol.span.empty() || !matches(symbol.name)) {
                        continue;
                    }

                    const source_file_t& file = compiled->sources.get(symbol.span.file);
                    if (file.path().starts_with("std/")) {
                        continue;
                    }

                    const std::uint32_t line = file.line_of(symbol.span.begin) + 1;
                    text +=
                        std::format("## {}\n({}:{}, in the package)\n\n{}\n\n", symbol.name,
                                    file.path(), line, first_line(compiled->sources, symbol.span));
                    ++found;
                }
            }
        }

        // std's, by their paths, and its built-in functions, which it doesn't declare.
        const docs_t& all = docs();
        for (const doc_entry_t& entry : all.entries) {
            if (!entry.where.starts_with("std/")) {
                continue;
            }

            // A dotted name is a path's end, like "String.Split", and a plain one a name.
            const std::string title = lowered(entry.title);
            const std::string wanted = lowered(name);
            if (title == wanted || title.ends_with(std::format(".{}", wanted))) {
                text += std::format("## {}\n({})\n\n{}\n\n", entry.title, entry.where, entry.text);
                ++found;
            }
        }

        if (const std::optional<compilation_t>& standard = all.standard) {
            for (const symbol_t& symbol : standard->analysis.symbols) {
                if (symbol.kind != symbol_kind_t::BUILTIN_FUNCTION || !matches(symbol.name)) {
                    continue;
                }

                std::string parameters;
                for (const symbol_id_t parameter : symbol.parameters) {
                    const symbol_t& taken =
                        standard->analysis.symbols[static_cast<std::size_t>(parameter)];
                    parameters += std::format("{}{}: {}{}", parameters.empty() ? "" : ", ",
                                              taken.name, standard->analysis.types.name(taken.type),
                                              taken.variadic ? "..." : "");
                }

                const std::string_view result = symbol.type == type_id_t::NONE
                                                    ? ""
                                                    : standard->analysis.types.name(symbol.type);
                text += std::format("## {}\n(built in)\n\nFunction {}({}){}{}\n\n", symbol.name,
                                    symbol.name, parameters, result.empty() ? "" : ": ", result);
                ++found;
            }
        }

        if (found == 0) {
            return {
                .text = std::format("Nothing in the package or std is named '{}'; "
                                    "search_docs searches by words.",
                                    name),
            };
        }

        return { .text = text };
    }

    [[nodiscard]] tool_result_t check(const json_t& arguments) const {
        const std::optional<std::string> code =
            arguments.contains("code") ? std::optional(arguments.at("code").get<std::string>())
                                       : std::nullopt;
        const std::optional<std::string> file =
            arguments.contains("file") ? std::optional(arguments.at("file").get<std::string>())
                                       : std::nullopt;
        // None of them runs a build step: each takes what the step's last run made.
        std::deque<std::expected<compilation_t, std::string>> compiled;
        if (file) {
            const std::expected<std::filesystem::path, std::string> path = resolved(*file);
            if (!path) {
                return { .text = path.error(), .error = true };
            }

            if (!code && !std::filesystem::is_regular_file(*path)) {
                return { .text = std::format("There's no file {}.", *file), .error = true };
            }

            check_request_t request{
                .files = { *path },
                .main = *path,
                .build_steps = build_steps_t::KEPT,
            };
            if (code) {
                request.read =
                    [&path, &text = *code](
                        const std::filesystem::path& read) -> std::optional<std::string> {
                    std::error_code error;
                    if (std::filesystem::equivalent(read, *path, error) ||
                        read.lexically_normal() == path->lexically_normal()) {
                        return text;
                    }

                    return std::nullopt;
                };
            }

            compiled.push_back(check_files(request));
        } else if (code) {
            compiled.emplace_back(compile("main.fb", *code));
        } else if (find_package(root)) {
            compiled.push_back(compile_file(root, build_mode_t::DEBUG, build_steps_t::KEPT));
        } else {
            // A folder of loose files: each is a program of its own.
            std::vector<std::filesystem::path> files;
            std::error_code error;
            for (std::filesystem::directory_iterator each(root, error), end; !error && each != end;
                 each.increment(error)) {
                if (each->path().extension() == ".fb" && each->is_regular_file(error)) {
                    files.push_back(each->path());
                }
            }

            std::ranges::sort(files);
            if (files.empty()) {
                return {
                    .text = "There's no package here, and no .fb file; check takes 'file' or "
                            "'code'.",
                    .error = true,
                };
            }

            for (const std::filesystem::path& each : files) {
                compiled.push_back(compile_file(each, build_mode_t::DEBUG, build_steps_t::KEPT));
            }
        }

        std::string text;
        std::size_t errors = 0;
        std::size_t warnings = 0;
        for (const std::expected<compilation_t, std::string>& each : compiled) {
            if (!each) {
                return { .text = each.error(), .error = true };
            }

            report(*each, text, errors, warnings);
        }

        if (errors == 0 && warnings == 0) {
            return { .text = "No errors or warnings." };
        }

        return {
            .text = std::format("{} error{}, {} warning{}.\n\n{}", errors, errors == 1 ? "" : "s",
                                warnings, warnings == 1 ? "" : "s", text),
        };
    }

    [[nodiscard]] tool_result_t format(const json_t& arguments) const {
        std::string path = "main.fb";
        std::string code;
        if (arguments.contains("code")) {
            code = arguments.at("code").get<std::string>();
        } else if (arguments.contains("file")) {
            const std::string given = arguments.at("file").get<std::string>();
            const std::expected<std::filesystem::path, std::string> file = resolved(given);
            if (!file) {
                return { .text = file.error(), .error = true };
            }

            std::ifstream in(*file, std::ios::binary);
            if (!in) {
                return { .text = std::format("There's no file {}.", given), .error = true };
            }

            code.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            path = to_utf8(file->filename());
        } else {
            return { .text = "format takes 'code' or 'file'.", .error = true };
        }

        const auto width = arguments.value<std::uint32_t>("width", default_format_width);
        source_manager_t sources;
        const file_id_t id = sources.add(path, code);
        const std::expected<std::string, std::vector<diagnostic_t>> formatted =
            format_source(sources.get(id), std::max<std::uint32_t>(width, 40));
        if (formatted) {
            return { .text = *formatted };
        }

        std::string text = "It doesn't parse, so it isn't formatted:\n\n";
        for (const diagnostic_t& diagnostic : formatted.error()) {
            text += render_pretty(diagnostic, sources);
        }

        return { .text = text, .error = true };
    }

    /// Answers the calls whose deadlines have come, until the server closes.
    void watch() {
        std::unique_lock lock(mutex);
        while (!closed) {
            const auto now = std::chrono::steady_clock::now();
            auto next = std::chrono::steady_clock::time_point::max();
            std::vector<std::pair<reply_t, json_t>> late;
            for (auto at = calls.begin(); at != calls.end();) {
                if (at->second.deadline > now) {
                    next = std::min(next, at->second.deadline);
                    ++at;
                    continue;
                }

                const tool_result_t expired{
                    .text = std::format(
                        "It didn't finish in {:g} s, so its answer isn't waited for; a check of a "
                        "Const that computes a lot, or of a large package, can take that long.",
                        static_cast<double>(options.deadline.count()) / 1000),
                    .error = true,
                };
                late.emplace_back(std::move(at->second.reply),
                                  success(at->second.id, text_result(expired)));
                at = calls.erase(at);
            }

            if (late.empty()) {
                if (next == std::chrono::steady_clock::time_point::max()) {
                    changed.wait(lock);
                } else {
                    changed.wait_until(lock, next);
                }

                continue;
            }

            replying += late.size();
            lock.unlock();
            for (auto& [reply, answer] : late) {
                reply(std::move(answer));
            }

            lock.lock();
            replying -= late.size();
            changed.notify_all();
        }
    }

    /// Answers the call `key` names with `result`, unless it was answered, it was cancelled, or
    /// the server closed.
    void finish(const std::string& key, json_t result) {
        std::unique_lock lock(mutex);
        const auto found = calls.find(key);
        if (closed || found == calls.end()) {
            return;
        }

        const reply_t reply = std::move(found->second.reply);
        json_t answer = success(found->second.id, std::move(result));
        calls.erase(found);
        ++replying;
        lock.unlock();
        reply(std::move(answer));
        lock.lock();
        --replying;
        changed.notify_all();
    }

    /// Leaves the call `key` names unanswered, which its batch, if it's in one, counts as done.
    void cancel(const std::string& key) {
        std::unique_lock lock(mutex);
        const auto found = calls.find(key);
        if (found == calls.end()) {
            return;
        }

        const reply_t reply = std::move(found->second.reply);
        calls.erase(found);
        ++replying;
        lock.unlock();
        reply(std::nullopt);
        lock.lock();
        --replying;
        changed.notify_all();
    }

    /// Starts a tool call on a thread of its own.
    void call(const json_t& id, const json_t& params, const reply_t& reply) {
        if (!params.contains("name") || !params.at("name").is_string()) {
            reply(failure(id, invalid_params, "tools/call needs the tool's name"));
            return;
        }

        std::string tool = params.at("name").get<std::string>();
        if (!std::ranges::contains(tool_names, tool)) {
            reply(failure(id, invalid_params, std::format("there's no tool named '{}'", tool)));
            return;
        }

        const json_t no_arguments = json_t::object();
        const json_t& arguments =
            params.contains("arguments") ? params.at("arguments") : no_arguments;
        if (!arguments.is_object()) {
            reply(failure(id, invalid_params, "a tool's arguments are an object"));
            return;
        }

        std::string key = id.dump();
        bool running = false;
        {
            const std::scoped_lock lock(mutex);
            running = calls.contains(key);
            if (!running) {
                calls.emplace(key,
                              call_t{
                                  .reply = reply,
                                  .id = id,
                                  .deadline = std::chrono::steady_clock::now() + options.deadline,
                              });
                changed.notify_all();
            }
        }

        if (running) {
            reply(failure(id, invalid_request, "a call with that id is running already"));
            return;
        }

        std::thread([core = shared_from_this(), key = std::move(key), tool = std::move(tool),
                     arguments = arguments] {
            json_t result;
            try {
                result = text_result(core->run(tool, arguments));
            } catch (const json_t::exception& error) {
                result = text_result({
                    .text = std::format("Its arguments aren't what it takes: {}", error.what()),
                    .error = true,
                });
            } catch (const std::exception& error) {
                result = text_result({ .text = error.what(), .error = true });
            }

            core->finish(key, std::move(result));
        }).detach();
    }

    /// Takes one message, and says whether it's answered, now or once a call is done.
    bool process(const json_t& message, const reply_t& reply) {
        if (!message.is_object()) {
            reply(failure(nullptr, invalid_request, "a message is a JSON-RPC object"));
            return true;
        }

        // A client's response, which a server that sends no requests has none to wait for.
        if (!message.contains("method") &&
            (message.contains("result") || message.contains("error"))) {
            return false;
        }

        const bool has_id = message.contains("id");
        const json_t no_id;
        const json_t& id = has_id ? message.at("id") : no_id;
        if (has_id && !id.is_string() && !id.is_number_integer()) {
            reply(
                failure(nullptr, invalid_request, "a request's id is a string or a whole number"));
            return true;
        }

        if (!message.contains("jsonrpc") || message.at("jsonrpc") != "2.0" ||
            !message.contains("method") || !message.at("method").is_string()) {
            reply(failure(id, invalid_request,
                          R"(a request has "jsonrpc": "2.0" and a method's name)"));
            return true;
        }

        const std::string method = message.at("method").get<std::string>();
        const json_t no_params = json_t::object();
        const json_t& params = message.contains("params") ? message.at("params") : no_params;
        if (!params.is_object()) {
            if (has_id) {
                reply(failure(id, invalid_params, "its params are an object"));
            }

            return has_id;
        }

        // A notification has no answer; one that cancels a call leaves its answer unsent.
        if (!has_id) {
            if (method == "notifications/cancelled" && params.contains("requestId")) {
                cancel(params.at("requestId").dump());
            }

            return false;
        }

        try {
            if (method == "initialize") {
                start_docs();
                const std::string asked = params.value("protocolVersion", std::string());
                version = std::ranges::contains(protocol_versions, asked)
                              ? asked
                              : std::string(protocol_versions.front());
                reply(success(
                    id, json_t{
                            { "protocolVersion", version },
                            {
                                "capabilities",
                                json_t{ { "tools", json_t{ { "listChanged", false } } } },
                            },
                            {
                                "serverInfo",
                                json_t{ { "name", "fb" }, { "version", build_config::version } },
                            },
                            { "instructions", agents_primer() },
                        }));
            } else if (method == "ping") {
                reply(success(id, json_t::object()));
            } else if (method == "tools/list") {
                reply(success(id, json_t{ { "tools", tools() } }));
            } else if (method == "tools/call") {
                call(id, params, reply);
            } else {
                reply(failure(id, method_not_found, std::format("there's no method '{}'", method)));
            }
        } catch (const json_t::exception& error) {
            reply(failure(id, invalid_params,
                          std::format("its params aren't what it takes: {}", error.what())));
        }

        return true;
    }
};

mcp_server_t::mcp_server_t(const std::filesystem::path& root, send_t send, mcp_options_t options)
    : m_core(std::make_shared<core_t>()) {
    m_core->root = std::filesystem::absolute(root).lexically_normal();
    m_core->send = std::move(send);
    m_core->options = options;
    m_core->deadlines = std::thread([core = m_core.get()] { core->watch(); });
}

mcp_server_t::~mcp_server_t() {
    {
        std::unique_lock lock(m_core->mutex);
        m_core->closed = true;
        m_core->calls.clear();
        m_core->changed.notify_all();
        m_core->changed.wait(lock, [this] { return m_core->replying == 0; });
    }

    m_core->deadlines.join();
}

void mcp_server_t::receive(const std::string_view line) {
    core_t& core = *m_core;
    const reply_t single = [&core](const std::optional<json_t>& answer) {
        if (answer) {
            core.write(*answer);
        }
    };
    if (too_deep(line)) {
        single(failure(nullptr, invalid_request,
                       std::format("a message nests at most {} deep", deepest_nesting)));
        return;
    }

    const json_t message = json_t::parse(line, nullptr, false);
    if (message.is_discarded()) {
        single(failure(nullptr, parse_error, "the message isn't JSON"));
        return;
    }

    if (!message.is_array()) {
        (void)core.process(message, single);
        return;
    }

    // A batch, which 2025-03-26 has and the others don't, is answered as one, without the
    // answers its notifications don't have.
    if (core.version != "2025-03-26") {
        single(failure(nullptr, invalid_request,
                       "a batch is MCP 2025-03-26's, which this initialize didn't agree on"));
        return;
    }

    if (message.empty()) {
        single(failure(nullptr, invalid_request, "a batch has a message"));
        return;
    }

    const auto batch = std::make_shared<core_t::batch_t>();
    const auto counted = [batch, &core](const std::optional<json_t>& answer, const bool added) {
        std::unique_lock lock(batch->mutex);
        if (answer) {
            batch->answers.push_back(*answer);
        }

        batch->waiting = added ? batch->waiting + 1 : batch->waiting - 1;
        if (batch->waiting == 0 && !batch->answers.empty()) {
            const json_t answers = std::move(batch->answers);
            lock.unlock();
            core.write(answers);
        }
    };
    const reply_t into = [counted](const std::optional<json_t>& answer) { counted(answer, false); };
    for (const json_t& each : message) {
        counted(std::nullopt, true);
        if (!core.process(each, into)) {
            counted(std::nullopt, false);
        }
    }

    counted(std::nullopt, false);
}

void mcp_server_t::settle() {
    std::unique_lock lock(m_core->mutex);
    m_core->changed.wait(lock, [this] { return m_core->calls.empty() && m_core->replying == 0; });
}

} // namespace fb
