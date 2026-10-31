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
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <iterator>
#include <nlohmann/json.hpp>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fb {

namespace {

using json = nlohmann::json;

/// MCP's versions this server speaks, the newest first, which it answers an initialize with when
/// the client's isn't one of them.
constexpr std::array<std::string_view, 3> protocol_versions{ "2025-06-18", "2025-03-26",
                                                             "2024-11-05" };

/// JSON-RPC's error codes.
constexpr int parse_error = -32700;
constexpr int invalid_request = -32600;
constexpr int method_not_found = -32601;
constexpr int invalid_params = -32602;
constexpr int internal_error = -32603;

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
    doc_entry_t entry{ .title = std::move(title),
                       .where = std::move(where),
                       .text = std::move(text) };
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

/// Words a search leaves out, which most of the docs have.
constexpr std::array<std::string_view, 15> filler_words{ "a",  "an", "and", "the", "to",
                                                         "of", "in", "is",  "it",  "how",
                                                         "do", "i",  "can", "on",  "or" };

/// A search's words: its letters, digits, and '_'s, in lower case, without its filler.
std::vector<std::string> words_of(const std::string_view query) {
    std::vector<std::string> words;
    std::string word;
    const auto add = [&] {
        if (!word.empty() && !std::ranges::contains(filler_words, word)) {
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

json text_result(const tool_result_t& result) {
    return json{
        { "content", json::array({ json{ { "type", "text" }, { "text", result.text } } }) },
        { "isError", result.error },
    };
}

/// A tool's description, for tools/list.
json tool(const std::string_view name, const std::string_view description, json properties,
          json required) {
    return json{
        { "name", name },
        { "description", description },
        { "inputSchema", json{ { "type", "object" },
                               { "properties", std::move(properties) },
                               { "required", std::move(required) } } },
    };
}

json tools() {
    return json::array({
        tool("search_docs",
             "Searches FutureBlitz's language spec and its standard library's reference for the "
             "words given, like 'For loop', 'string split', or 'Map', and gives the sections and "
             "declarations that have them, best first.",
             json{ { "query",
                     json{ { "type", "string" }, { "description", "Words to search for" } } },
                   { "limit", json{ { "type", "integer" },
                                    { "description", "How many results, 6 unless it says" } } } },
             json::array({ "query" })),
        tool("lookup",
             "Looks up a name in the package and the standard library, like 'Split', "
             "'std.collections.map.Map', or one of the package's own Functions, and gives each "
             "declaration of it: its first line, where it is, and its doc.",
             json{ { "name", json{ { "type", "string" },
                                   { "description", "The name, or a dotted path" } } } },
             json::array({ "name" })),
        tool("check",
             "Checks FutureBlitz code for errors without building it, and gives each error and "
             "warning with its line, its fix when it has one, and its notes. With 'code' alone, it "
             "checks that as a program of its own; with 'file', that file of the package, with "
             "'code' as its text if it's given, though it isn't saved; with neither, the whole "
             "package.",
             json{ { "code", json{ { "type", "string" }, { "description", "FutureBlitz code" } } },
                   { "file", json{ { "type", "string" },
                                   { "description", "A .fb file, from the package's folder" } } } },
             json::array()),
        tool("format",
             "Formats FutureBlitz code as fb fmt does, and gives it back, or the errors that keep "
             "it from being formatted. It writes nothing.",
             json{ { "code", json{ { "type", "string" }, { "description", "FutureBlitz code" } } },
                   { "file", json{ { "type", "string" },
                                   { "description", "A .fb file, from the package's folder" } } },
                   { "width", json{ { "type", "integer" },
                                    { "description", "The line width, 100 unless it says" } } } },
             json::array()),
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
struct docs_t {
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
        std::string text = module.doc ? module.doc->markdown : std::string();
        docs.entries.push_back(entry_of(module.name, std::format("std/{}", module.file), text));
        add_items(module.items, docs.entries);
    }

    docs.standard = std::move(*compiled);
    return docs;
}

} // namespace

struct mcp_server_t::state_t {
    std::filesystem::path root;
    /// The docs, which an initialize starts reading on a thread of their own, as reading std
    /// takes seconds, or the first tool that needs them reads.
    std::future<docs_t> loading;
    std::optional<docs_t> loaded;

    const docs_t& docs() {
        if (!loaded) {
            loaded = loading.valid() ? loading.get() : load_docs();
        }

        return *loaded;
    }

    /// A path a tool names, from the package's folder.
    [[nodiscard]] std::filesystem::path resolved(const std::string_view given) const {
        const std::filesystem::path path = path_from_utf8(given);
        return path.is_absolute() ? path : root / path;
    }

    tool_result_t search(const json& arguments) {
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
            return { .text =
                         std::format("Nothing in the spec or std's reference has '{}'.", query) };
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

    tool_result_t lookup(const json& arguments) {
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
        // The package's own declarations, from a check of every file of it.
        if (find_package(root)) {
            if (std::expected<compilation_t, std::string> compiled = compile_docs(root)) {
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
            return { .text = std::format("Nothing in the package or std is named '{}'; "
                                         "search_docs searches by words.",
                                         name) };
        }

        return { .text = text };
    }

    tool_result_t check(const json& arguments) {
        const std::optional<std::string> code =
            arguments.contains("code") ? std::optional(arguments.at("code").get<std::string>())
                                       : std::nullopt;
        const std::optional<std::string> file =
            arguments.contains("file") ? std::optional(arguments.at("file").get<std::string>())
                                       : std::nullopt;
        std::expected<compilation_t, std::string> compiled = std::unexpected(std::string());
        if (file) {
            const std::filesystem::path path = resolved(*file);
            if (!code && !std::filesystem::is_regular_file(path)) {
                return { .text = std::format("There's no file at {}.", to_utf8(path)),
                         .error = true };
            }

            check_request_t request{ .files = { path }, .main = path };
            if (code) {
                request.read =
                    [path, text = *code](
                        const std::filesystem::path& read) -> std::optional<std::string> {
                    std::error_code error;
                    if (std::filesystem::equivalent(read, path, error) ||
                        read.lexically_normal() == path.lexically_normal()) {
                        return text;
                    }

                    return std::nullopt;
                };
            }

            compiled = check_files(request);
        } else if (code) {
            compiled = compile("main.fb", *code);
        } else {
            compiled = compile_file(root);
        }

        if (!compiled) {
            return { .text = compiled.error(), .error = true };
        }

        std::string text;
        std::size_t errors = 0;
        std::size_t warnings = 0;
        for (const diagnostic_t& diagnostic : compiled->diagnostics.diagnostics()) {
            (diagnostic.severity == severity_t::ERROR ? errors : warnings) += 1;
            text += render_pretty(diagnostic, compiled->sources);
            for (const fix_t& fix : diagnostic.fixes) {
                const source_file_t& in = compiled->sources.get(fix.span.file);
                text += std::format("  fix: at line {}, put `{}` in place of `{}`\n",
                                    in.line_of(fix.span.begin) + 1, fix.replacement,
                                    in.text().substr(fix.span.begin, fix.span.length()));
            }

            text += '\n';
        }

        if (errors == 0 && warnings == 0) {
            return { .text = "No errors or warnings." };
        }

        return { .text =
                     std::format("{} error{}, {} warning{}.\n\n{}", errors, errors == 1 ? "" : "s",
                                 warnings, warnings == 1 ? "" : "s", text) };
    }

    tool_result_t format(const json& arguments) {
        std::string path = "main.fb";
        std::string code;
        if (arguments.contains("code")) {
            code = arguments.at("code").get<std::string>();
        } else if (arguments.contains("file")) {
            const std::filesystem::path file = resolved(arguments.at("file").get<std::string>());
            std::ifstream in(file, std::ios::binary);
            if (!in) {
                return { .text = std::format("There's no file at {}.", to_utf8(file)),
                         .error = true };
            }

            code.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            path = to_utf8(file.filename());
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
};

mcp_server_t::mcp_server_t(std::filesystem::path root) : m_state(std::make_unique<state_t>()) {
    m_state->root = std::filesystem::absolute(root).lexically_normal();
}

mcp_server_t::mcp_server_t(mcp_server_t&&) noexcept = default;
mcp_server_t& mcp_server_t::operator=(mcp_server_t&&) noexcept = default;
mcp_server_t::~mcp_server_t() = default;

std::optional<std::string> mcp_server_t::handle(const std::string_view line) {
    const auto failed = [](const json& id, const int code, const std::string_view message) {
        return json{
            { "jsonrpc", "2.0" },
            { "id", id },
            { "error", json{ { "code", code }, { "message", message } } }
        }.dump(-1, ' ', false, json::error_handler_t::replace);
    };
    const json message = json::parse(line, nullptr, false);
    if (message.is_discarded()) {
        return failed(nullptr, parse_error, "the message isn't JSON");
    }

    if (!message.is_object() || !message.contains("method") || !message.at("method").is_string()) {
        return failed(message.is_object() ? message.value("id", json()) : json(), invalid_request,
                      "the message isn't a JSON-RPC request");
    }

    const std::string method = message.at("method").get<std::string>();
    // A notification, like notifications/initialized, has no id and no answer.
    if (!message.contains("id")) {
        return std::nullopt;
    }

    const json& id = message.at("id");
    const json params = message.value("params", json::object());
    const auto answered = [&](json result) {
        return json{ { "jsonrpc", "2.0" }, { "id", id }, { "result", std::move(result) } }.dump(
            -1, ' ', false, json::error_handler_t::replace);
    };
    try {
        if (method == "initialize") {
            if (!m_state->loaded && !m_state->loading.valid()) {
                m_state->loading = std::async(std::launch::async, load_docs);
            }

            const std::string asked = params.value("protocolVersion", std::string());
            const bool known = std::ranges::contains(protocol_versions, asked);
            return answered(json{
                { "protocolVersion", known ? asked : std::string(protocol_versions.front()) },
                { "capabilities", json{ { "tools", json{ { "listChanged", false } } } } },
                { "serverInfo", json{ { "name", "fb" }, { "version", build_config::version } } },
                { "instructions", agents_primer() },
            });
        }

        if (method == "ping") {
            return answered(json::object());
        }

        if (method == "tools/list") {
            return answered(json{ { "tools", tools() } });
        }

        if (method == "tools/call") {
            const std::string name = params.at("name").get<std::string>();
            const json arguments = params.value("arguments", json::object());
            if (name == "search_docs") {
                return answered(text_result(m_state->search(arguments)));
            }

            if (name == "lookup") {
                return answered(text_result(m_state->lookup(arguments)));
            }

            if (name == "check") {
                return answered(text_result(m_state->check(arguments)));
            }

            if (name == "format") {
                return answered(text_result(m_state->format(arguments)));
            }

            return failed(id, invalid_params, std::format("there's no tool named '{}'", name));
        }

        return failed(id, method_not_found, std::format("there's no method '{}'", method));
    } catch (const json::exception& error) {
        return failed(id, invalid_params,
                      std::format("its params aren't what it takes: {}", error.what()));
    } catch (const std::exception& error) {
        return failed(id, internal_error, error.what());
    }
}

} // namespace fb
