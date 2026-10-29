#include "fb/doc.hpp"

#include "fb/build_config.hpp"
#include "fb/driver.hpp"
#include "fb/lexer.hpp"
#include "fb/parser.hpp"
#include "fb/types.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace fb {
namespace {

// Markdown

[[nodiscard]] std::string_view trim(const std::string_view text) {
    const std::size_t begin = text.find_first_not_of(" \t");
    if (begin == std::string_view::npos) {
        return {};
    }

    return text.substr(begin, text.find_last_not_of(" \t") - begin + 1);
}

[[nodiscard]] bool blank(const std::string_view line) {
    return trim(line).empty();
}

[[nodiscard]] bool name_character(const char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/// A line that opens or closes a fenced code block: up to 3 spaces, then 3 or more '`' or '~',
/// and for an opening one, its info string.
struct fence_t {
    char mark = '`';
    std::size_t length = 0;
    std::size_t indent = 0;
    std::string_view info;
};

[[nodiscard]] std::optional<fence_t> fence_of(const std::string_view line) {
    // A blank line's npos is more than 3 too.
    const std::size_t indent = line.find_first_not_of(' ');
    if (indent > 3 || (line[indent] != '`' && line[indent] != '~')) {
        return std::nullopt;
    }

    const char mark = line[indent];
    const std::size_t end = std::min(line.find_first_not_of(mark, indent), line.size());
    const std::string_view info = trim(line.substr(end));
    if (end - indent < 3 || (mark == '`' && info.contains('`'))) {
        return std::nullopt;
    }

    return fence_t{ .mark = mark, .length = end - indent, .indent = indent, .info = info };
}

/// An ATX heading's text: up to 3 spaces, 1 to 6 '#', then a space or the end, without the '#'s
/// that close it.
[[nodiscard]] std::optional<std::string_view> heading_of(const std::string_view line) {
    const std::size_t indent = line.find_first_not_of(' ');
    if (indent > 3 || line[indent] != '#') {
        return std::nullopt;
    }

    const std::size_t end = std::min(line.find_first_not_of('#', indent), line.size());
    if (end - indent > 6 || (end < line.size() && line[end] != ' ' && line[end] != '\t')) {
        return std::nullopt;
    }

    std::string_view text = trim(line.substr(end));
    const std::size_t closing = text.find_last_not_of('#');
    if (closing == std::string_view::npos) {
        return std::string_view();
    }

    if (closing + 1 < text.size() && (text[closing] == ' ' || text[closing] == '\t')) {
        text = trim(text.substr(0, closing + 1));
    }

    return text;
}

/// What a link's text names, when it's a name, a dotted one, or one in backticks, with a "()"
/// or type arguments after it, which it leaves out: "Stack.Push" for "`Stack.Push()`".
[[nodiscard]] std::optional<std::string> link_target(std::string_view inner) {
    if (inner.size() >= 2 && inner.front() == '`' && inner.back() == '`') {
        inner = inner.substr(1, inner.size() - 2);
    }

    if (inner.ends_with("()")) {
        inner.remove_suffix(2);
    }

    // Type arguments, like the "<T>" in "Stack<T>.Push", which can nest.
    std::string target;
    std::size_t depth = 0;
    for (const char c : inner) {
        if (c == '<') {
            ++depth;
        } else if (c == '>') {
            if (depth == 0) {
                return std::nullopt;
            }

            --depth;
        } else if (depth == 0) {
            target += c;
        }
    }

    if (depth != 0 || target.empty()) {
        return std::nullopt;
    }

    for (const auto part : std::views::split(target, '.')) {
        const std::string_view name(part.begin(), part.end());
        if (name.empty() || (name.front() >= '0' && name.front() <= '9') ||
            !std::ranges::all_of(name, name_character)) {
            return std::nullopt;
        }
    }

    return target;
}

/// How many backticks open a code span after a run of `run` of them, when `open` did before, or
/// 0 for none: a run opens one, and closes one it's as long as.
[[nodiscard]] std::size_t ticks_after(const std::size_t open, const std::size_t run) {
    if (open == 0) {
        return run;
    }

    return open == run ? 0 : open;
}

/// A paragraph's first sentence: up to a '.', '!', or '?' outside code that a space follows, or
/// that ends it.
[[nodiscard]] std::string first_sentence(const std::string_view paragraph) {
    std::size_t ticks = 0;
    for (std::size_t i = 0; i < paragraph.size();) {
        const char c = paragraph[i];
        if (c == '`') {
            const std::size_t run =
                std::min(paragraph.find_first_not_of('`', i), paragraph.size()) - i;
            ticks = ticks_after(ticks, run);
            i += run;
            continue;
        }

        if (c == '\\') {
            i += 2;
            continue;
        }

        if (ticks == 0 && (c == '.' || c == '!' || c == '?') &&
            (i + 1 == paragraph.size() || paragraph[i + 1] == ' ')) {
            return std::string(paragraph.substr(0, i + 1));
        }

        ++i;
    }

    return std::string(paragraph);
}

/// Reads a doc comment line by line, as its Markdown goes.
class doc_reader_t {
public:
    explicit doc_reader_t(const doc_comment_t& comment) : m_comment(comment) {}

    doc_t run() {
        std::vector<std::string_view> lines;
        for (const auto part : std::views::split(std::string_view(m_comment.text), '\n')) {
            lines.emplace_back(part.begin(), part.end());
        }

        m_doc.sections.push_back({});
        for (std::size_t i = 0; i < lines.size() && i < m_comment.lines.size(); ++i) {
            read_line(lines[i], m_comment.lines[i]);
        }

        // A code block that's still open at the end runs to it.
        finish_example();
        std::erase_if(m_doc.sections, [](doc_section_t& section) {
            section.markdown = std::string(trim_lines(section.markdown));
            return section.heading.empty() && section.markdown.empty();
        });
        m_doc.markdown = std::string(trim_lines(m_doc.markdown));
        m_doc.summary = first_sentence(m_paragraph);
        return std::move(m_doc);
    }

private:
    const doc_comment_t& m_comment;
    doc_t m_doc;
    /// The code block that's open, and whether it's an example.
    std::optional<fence_t> m_fence;
    std::optional<doc_example_t> m_example;
    /// The first paragraph, its lines joined by spaces, and whether it's still going, or done.
    std::string m_paragraph;
    bool m_in_paragraph = false;
    bool m_summarized = false;
    /// How many backticks open the code span a line ended inside, or 0.
    std::size_t m_ticks = 0;

    /// Without the blank lines at its start and its end.
    [[nodiscard]] static std::string_view trim_lines(std::string_view text) {
        while (!text.empty()) {
            const std::size_t end = text.find('\n');
            if (!blank(text.substr(0, end)) || end == std::string_view::npos) {
                break;
            }

            text.remove_prefix(end + 1);
        }

        while (!text.empty()) {
            const std::size_t start = text.rfind('\n');
            const std::string_view last =
                start == std::string_view::npos ? text : text.substr(start + 1);
            if (!blank(last)) {
                break;
            }

            text = start == std::string_view::npos ? std::string_view() : text.substr(0, start);
        }

        return text;
    }

    void show(const std::string_view line, const bool in_section = true) {
        m_doc.markdown += std::string(m_doc.markdown.empty() ? "" : "\n") + std::string(line);
        if (in_section) {
            std::string& markdown = m_doc.sections.back().markdown;
            markdown += std::string(markdown.empty() ? "" : "\n") + std::string(line);
        }
    }

    /// The first paragraph ends at a blank line, a heading, or a code block, and the summary is
    /// in it, or nowhere when something else comes first.
    void end_paragraph() {
        m_in_paragraph = false;
        m_summarized = true;
        m_ticks = 0;
    }

    void finish_example() {
        if (m_example) {
            m_doc.examples.push_back(std::move(*m_example));
            m_example.reset();
        }
    }

    [[nodiscard]] bool in_examples() const {
        const std::string& heading = m_doc.sections.back().heading;
        return heading == "Examples" || heading == "Example";
    }

    void read_line(const std::string_view line, const span_t span) {
        if (const std::optional<fence_t> open = m_fence) {
            read_code(line, span, *open);
            return;
        }

        if (const std::optional<fence_t> fence = fence_of(line)) {
            end_paragraph();
            m_fence = fence;
            const bool fb =
                fence->info.empty() || fence->info == "fb" || fence->info == "fb no_run";
            if (in_examples() && fb) {
                m_example = doc_example_t{
                    .kind =
                        fence->info == "fb no_run" ? example_kind_t::NO_RUN : example_kind_t::RUN,
                    .fence = span,
                };
            }

            show(line);
            return;
        }

        if (const std::optional<std::string_view> heading = heading_of(line)) {
            end_paragraph();
            show(line, false);
            m_doc.sections.push_back({ .heading = std::string(*heading) });
            return;
        }

        if (blank(line)) {
            if (m_in_paragraph) {
                end_paragraph();
            }

            m_ticks = 0;
            show(line);
            return;
        }

        if (!m_summarized) {
            m_in_paragraph = true;
            m_summarized = true;
        }

        if (m_in_paragraph) {
            m_paragraph += std::string(m_paragraph.empty() ? "" : " ") + std::string(trim(line));
        }

        find_links(line, span);
        show(line);
    }

    /// A line in the code block `open` opened: the end of it, or its code, which in an example can
    /// be hidden.
    void read_code(const std::string_view line, const span_t span, const fence_t& open) {
        const std::optional<fence_t> closing = fence_of(line);
        if (closing && closing->mark == open.mark && closing->length >= open.length &&
            closing->info.empty()) {
            m_fence.reset();
            finish_example();
            show(line);
            return;
        }

        // The code goes in as far as the fence, as Markdown has it.
        std::size_t start = 0;
        while (start < open.indent && start < line.size() && line[start] == ' ') {
            ++start;
        }

        if (!m_example) {
            show(line);
            return;
        }

        // "# code", or "#" alone, is hidden, as Rust's examples have it; "#If" is code.
        const std::string_view code = line.substr(start);
        const std::size_t text = code.find_first_not_of(" \t");
        const std::string_view rest =
            text == std::string_view::npos ? std::string_view() : code.substr(text);
        const bool hidden = rest == "#" || rest.starts_with("# ");
        if (hidden) {
            start += text + std::min<std::size_t>(rest.size(), 2);
        }

        const auto begin = span.begin + static_cast<std::uint32_t>(start);
        m_example->lines.push_back({
            .text = std::string(line.substr(start)),
            .span = { .file = span.file, .begin = begin, .end = std::max(begin, span.end) },
            .hidden = hidden,
        });
        if (!hidden) {
            show(line);
        }
    }

    /// The "[Name]" links in a line of text, outside code spans, which can go on from the line
    /// before, and but for Markdown's own links, "[text](url)" and "[text][label]".
    void find_links(const std::string_view line, const span_t span) {
        for (std::size_t i = 0; i < line.size();) {
            const char c = line[i];
            if (c == '\\') {
                i += 2;
                continue;
            }

            if (c == '`') {
                const std::size_t run = std::min(line.find_first_not_of('`', i), line.size()) - i;
                m_ticks = ticks_after(m_ticks, run);
                i += run;
                continue;
            }

            // A '[' right after a name or a bracket indexes, as "a[i]" does, and "![" is an image.
            const bool opens = c == '[' && m_ticks == 0 &&
                               (i == 0 || (!name_character(line[i - 1]) && line[i - 1] != ']' &&
                                           line[i - 1] != ')' && line[i - 1] != '!'));
            const std::size_t close = opens ? line.find(']', i + 1) : std::string_view::npos;
            if (close == std::string_view::npos) {
                ++i;
                continue;
            }

            // "[label]: url" at the start of a line defines a link.
            const char after = close + 1 < line.size() ? line[close + 1] : '\0';
            const bool definition =
                after == ':' && line.substr(0, i).find_first_not_of(' ') == std::string_view::npos;
            if (after != '(' && after != '[' && !definition) {
                if (std::optional<std::string> target =
                        link_target(line.substr(i + 1, close - i - 1))) {
                    m_doc.links.push_back({
                        .target = std::move(*target),
                        .span = {
                            .file = span.file,
                            .begin = span.begin + static_cast<std::uint32_t>(i),
                            .end = span.begin + static_cast<std::uint32_t>(close + 1),
                        },
                    });
                }
            }

            i = close + 1;
        }
    }
};

} // namespace

std::string doc_example_t::shown() const {
    std::string code;
    for (const example_line_t& line : lines) {
        if (!line.hidden) {
            code += line.text + "\n";
        }
    }

    return code;
}

doc_t read_doc(const doc_comment_t& comment) {
    return doc_reader_t(comment).run();
}

namespace {

// Examples

/// Where an example's code goes in its module.
enum class part_t : std::uint8_t {
    IMPORT,
    DECLARATION,
    CODE,
};

/// What part of its module a top-level statement of an example is in.
[[nodiscard]] part_t part_of(const stmt_node_t& node) {
    if (std::holds_alternative<import_stmt_t>(node)) {
        return part_t::IMPORT;
    }

    if (const auto* variable = std::get_if<variable_stmt_t>(&node)) {
        return variable->kind == variable_kind_t::GLOBAL ? part_t::DECLARATION : part_t::CODE;
    }

    const bool declares =
        std::holds_alternative<function_stmt_t>(node) ||
        std::holds_alternative<type_stmt_t>(node) || std::holds_alternative<enum_stmt_t>(node) ||
        std::holds_alternative<alias_stmt_t>(node) || std::holds_alternative<extend_stmt_t>(node) ||
        std::holds_alternative<extern_stmt_t>(node);
    return declares ? part_t::DECLARATION : part_t::CODE;
}

/// The module of one example: its lines in the order they go, and where each is in the file.
class example_builder_t {
public:
    example_builder_t(const source_file_t& file, const doc_example_t& example)
        : m_file(file), m_example(example),
          m_fence(file.location_of(example.fence.begin, column_unit_t::UTF8)) {}

    example_module_t run(std::string name, const std::vector<std::string_view>& imports) {
        // The example's code as a file of its own, whose lines are its lines, to find its parts.
        std::string code;
        for (const example_line_t& line : m_example.lines) {
            code += line.text + "\n";
        }

        const source_file_t parsed_file(file_id_t{}, m_file.path(), code);
        diagnostic_bag_t ignored;
        const ast_t ast = parse(parsed_file, lex(parsed_file, ignored), ignored);
        std::vector<part_t> parts(m_example.lines.size(), part_t::CODE);
        std::vector<bool> placed(m_example.lines.size());
        std::vector<std::string> written;
        for (const stmt_id_t id : ast.items()) {
            const stmt_t& stmt = ast.get(id);
            if (const auto* imported = std::get_if<import_stmt_t>(&stmt.node)) {
                written.push_back(imported->target);
            }

            const std::uint32_t first =
                parsed_file.location_of(stmt.span.begin, column_unit_t::UTF8).line;
            const std::uint32_t last =
                parsed_file.location_of(stmt.span.end, column_unit_t::UTF8).line;
            // The lines before it that no statement has, comments and blank ones, go with it.
            std::uint32_t from = first;
            while (from > 0 && !placed[from - 1]) {
                --from;
            }

            for (std::uint32_t line = from; line <= last && line < parts.size(); ++line) {
                parts[line] = part_of(stmt.node);
                placed[line] = true;
            }
        }

        for (const std::string_view module : imports) {
            if (!std::ranges::contains(written, module)) {
                add(std::format("Import {}", module), m_fence);
            }
        }

        add_part(part_t::IMPORT, parts);
        add_part(part_t::DECLARATION, parts);
        // A test that only compiles has its code in a Function that nothing calls.
        if (m_example.kind == example_kind_t::RUN) {
            add("@Test Function Example()", m_fence);
        } else {
            add("Function Example()", m_fence);
        }

        add_part(part_t::CODE, parts);
        add("End Function", m_fence);
        if (m_example.kind == example_kind_t::NO_RUN) {
            add("@Test Function Compiles()", m_fence);
            add("End Function", m_fence);
        }

        // The end of the file, after the last line break, has a line of its own.
        m_module.lines.push_back(m_fence);
        m_module.name = std::move(name);
        return std::move(m_module);
    }

private:
    const source_file_t& m_file;
    const doc_example_t& m_example;
    location_t m_fence;
    example_module_t m_module;

    void add(const std::string_view text, const location_t origin) {
        m_module.text += std::string(text) + "\n";
        m_module.lines.push_back(origin);
    }

    void add_part(const part_t part, const std::vector<part_t>& parts) {
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (parts[i] == part) {
                const example_line_t& line = m_example.lines[i];
                add(line.text, m_file.location_of(line.span.begin, column_unit_t::UTF8));
            }
        }
    }
};

/// A declaration with a doc comment, and its path in its module, like "Stack.Push".
struct documented_t {
    std::string path;
    const doc_comment_t* doc = nullptr;
};

/// The doc comments of a file's declarations and their members, in source order.
std::vector<documented_t> documented(const ast_t& ast) {
    std::vector<documented_t> found;
    const auto note = [&](const std::string& path, const doc_comment_t* doc) {
        if (doc != nullptr) {
            found.push_back({ .path = path, .doc = doc });
        }
    };
    const auto member_name = [](const stmt_node_t& node) {
        const std::optional<name_t> name = declared_name(node);
        const auto* function = std::get_if<function_stmt_t>(&node);
        const std::string text(name ? name->text : std::string_view());
        return function != nullptr && function->kind == function_kind_t::OPERATOR
                   ? "Operator" + text
                   : text;
    };

    for (const stmt_id_t id : ast.items()) {
        const stmt_node_t& node = ast.get(id).node;
        if (const std::optional<name_t> name = declared_name(node)) {
            note(std::string(name->text), ast.doc(name->span));
        }

        if (const auto* type = std::get_if<type_stmt_t>(&node)) {
            for (const stmt_id_t member : type->members) {
                note(std::format("{}.{}", type->name.text, member_name(ast.get(member).node)),
                     ast.doc(member));
            }
        } else if (const auto* declared = std::get_if<enum_stmt_t>(&node)) {
            for (const enum_member_t& member : declared->members) {
                note(std::format("{}.{}", declared->name.text, member.name.text),
                     ast.doc(member.name.span));
            }
        } else if (const auto* extension = std::get_if<extend_stmt_t>(&node)) {
            const auto* named = std::get_if<named_type_t>(&ast.get(extension->type).node);
            const std::string_view extended = named != nullptr ? named->name : "T[]";
            for (const stmt_id_t member : extension->members) {
                note(std::format("{}.{}", extended, member_name(ast.get(member).node)),
                     ast.doc(member));
            }
        } else if (const auto* block = std::get_if<extern_stmt_t>(&node)) {
            for (const stmt_id_t function : block->functions) {
                note(member_name(ast.get(function).node), ast.doc(function));
            }
        }
    }

    return found;
}

} // namespace

std::vector<example_module_t> example_modules(const ast_t& ast, const source_file_t& file,
                                              const std::string_view module,
                                              const std::string_view import) {
    std::vector<std::string_view> imports{ "std.core", "std.test" };
    if (!std::ranges::contains(imports, import)) {
        imports.push_back(import);
    }

    std::vector<example_module_t> modules;
    for (const auto& [path, comment] : documented(ast)) {
        const doc_t doc = read_doc(*comment);
        for (std::size_t i = 0; i < doc.examples.size(); ++i) {
            std::string name = std::format("{}.{} (example{})", module, path,
                                           i == 0 ? std::string() : std::format(" {}", i + 1));
            modules.push_back(
                example_builder_t(file, doc.examples[i]).run(std::move(name), imports));
        }
    }

    return modules;
}

namespace {

// The API

/// Where a declaration is, for finding the names its doc's links name: its module, and the type
/// or Extend it's in, or for a type, itself.
struct scope_t {
    module_id_t module{};
    std::optional<stmt_id_t> type;
    std::optional<stmt_id_t> extension;
};

/// What a link's name, or the part of it so far, is.
struct found_t {
    std::string path;
    /// A declaration, whose members the rest of the name can name.
    std::optional<stmt_id_t> declaration;
    /// A module qualifier's modules, or a dotted module name's.
    std::vector<module_id_t> modules;
    /// A built-in type or Function, by its name.
    bool builtin = false;
};

/// Each declaration of a program, by name, with its path, for fb doc's items and links.
class api_index_t {
public:
    explicit api_index_t(const compilation_t& compilation) : m_ast(compilation.ast) {
        const module_table_t& modules = m_ast.modules();
        m_items.resize(modules.size());
        for (const stmt_id_t id : m_ast.items()) {
            if (const std::optional<module_id_t> module = modules.of(m_ast.get(id).span.file)) {
                m_items[std::to_underlying(*module)].push_back(id);
            }
        }

        m_top.resize(modules.size());
        for (std::size_t i = 0; i < modules.size(); ++i) {
            index_module(static_cast<module_id_t>(i));
        }

        for (const symbol_t& symbol : compilation.analysis.symbols) {
            if (symbol.kind == symbol_kind_t::BUILTIN_FUNCTION) {
                m_builtins.insert(symbol.name);
            }
        }
    }

    [[nodiscard]] const std::vector<stmt_id_t>& items(const module_id_t module) const {
        return m_items[std::to_underlying(module)];
    }

    [[nodiscard]] const std::string& path(const stmt_id_t id) const {
        static const std::string none;
        const auto found = m_paths.find(id);
        return found != m_paths.end() ? found->second : none;
    }

    [[nodiscard]] std::optional<scope_t> scope(const stmt_id_t id) const {
        const auto found = m_scopes.find(id);
        return found != m_scopes.end() ? std::optional(found->second) : std::nullopt;
    }

    /// An Extend's type as written without its type arguments: "Stack", "String", or "T[]".
    [[nodiscard]] std::string extended_name(const extend_stmt_t& extension) const {
        const type_expr_node_t& type = m_ast.get(extension.type).node;
        if (const auto* named = std::get_if<named_type_t>(&type)) {
            return std::string(named->name);
        }

        return "T[]";
    }

    /// The declaration or built-in an Extend extends, as its module finds it.
    [[nodiscard]] std::optional<found_t> extended(const stmt_id_t id) const {
        const auto& extension = std::get<extend_stmt_t>(m_ast.get(id).node);
        const auto* named = std::get_if<named_type_t>(&m_ast.get(extension.type).node);
        if (named == nullptr) {
            return found_t{ .path = "T[]", .builtin = true };
        }

        const std::optional<scope_t> where = scope(id);
        if (!where) {
            return std::nullopt;
        }

        std::optional<found_t> found;
        if (named->qualifier) {
            found = in_scope(named->qualifier->text, { .module = where->module });
            if (found) {
                found = member(*found, named->name, where->module);
            }
        } else {
            found = in_scope(named->name, { .module = where->module });
        }

        return found;
    }

    /// What a link names, as code in `scope` finds it.
    [[nodiscard]] std::optional<found_t> resolve(const std::string_view link,
                                                 const scope_t& scope) const {
        std::vector<std::string_view> parts;
        for (const auto part : std::views::split(link, '.')) {
            parts.emplace_back(part.begin(), part.end());
        }

        // A module's whole name, like "std.collections.stack.Stack", or its folder's.
        std::optional<found_t> found;
        std::size_t next = 1;
        for (std::size_t length = parts.size(); length > 1 && !found; --length) {
            std::string prefix(parts.front());
            for (std::size_t i = 1; i < length; ++i) {
                prefix += std::format(".{}", parts[i]);
            }

            if (std::vector<module_id_t> named = modules_named(prefix); !named.empty()) {
                found = found_t{ .path = prefix, .modules = std::move(named) };
                next = length;
            }
        }

        if (!found) {
            found = in_scope(parts.front(), scope);
        }

        for (std::size_t i = next; found && i < parts.size(); ++i) {
            found = member(*found, parts[i], scope.module);
        }

        return found;
    }

private:
    const ast_t& m_ast;
    std::vector<std::vector<stmt_id_t>> m_items;
    /// Each module's top-level declarations by name, Extern blocks' Functions among them.
    std::vector<std::unordered_map<std::string_view, std::vector<stmt_id_t>>> m_top;
    std::unordered_map<stmt_id_t, std::string> m_paths;
    std::unordered_map<stmt_id_t, scope_t> m_scopes;
    std::set<std::string_view> m_builtins;

    void index_declaration(const stmt_id_t id, const std::string& path, const scope_t& scope) {
        m_paths.emplace(id, path);
        m_scopes.emplace(id, scope);
    }

    [[nodiscard]] static std::string member_name(const stmt_node_t& node) {
        const std::optional<name_t> name = declared_name(node);
        const std::string text(name ? name->text : std::string_view());
        const auto* function = std::get_if<function_stmt_t>(&node);
        return function != nullptr && function->kind == function_kind_t::OPERATOR
                   ? "Operator" + text
                   : text;
    }

    void index_module(const module_id_t module) {
        const std::string& name = m_ast.modules().get(module).name;
        auto& top = m_top[std::to_underlying(module)];
        for (const stmt_id_t id : items(module)) {
            const stmt_node_t& node = m_ast.get(id).node;
            if (const std::optional<name_t> declared = declared_name(node)) {
                top[declared->text].push_back(id);
                const std::string path = std::format("{}.{}", name, declared->text);
                const bool is_type = std::holds_alternative<type_stmt_t>(node) ||
                                     std::holds_alternative<enum_stmt_t>(node);
                index_declaration(
                    id, path,
                    { .module = module, .type = is_type ? std::optional(id) : std::nullopt });
                if (const auto* type = std::get_if<type_stmt_t>(&node)) {
                    for (const stmt_id_t member : type->members) {
                        index_declaration(
                            member, std::format("{}.{}", path, member_name(m_ast.get(member).node)),
                            { .module = module, .type = id });
                    }
                }
            } else if (const auto* extension = std::get_if<extend_stmt_t>(&node)) {
                // An Extend's members are its type's, in its module; it's Extend and its type, as
                // no declaration can be named Extend.
                const std::string path = std::format("{}.{}", name, extended_name(*extension));
                index_declaration(id, std::format("{}.Extend.{}", name, extended_name(*extension)),
                                  { .module = module, .extension = id });
                for (const stmt_id_t member : extension->members) {
                    index_declaration(
                        member, std::format("{}.{}", path, member_name(m_ast.get(member).node)),
                        { .module = module, .extension = id });
                }
            } else if (const auto* block = std::get_if<extern_stmt_t>(&node)) {
                for (const stmt_id_t function : block->functions) {
                    const std::string text = member_name(m_ast.get(function).node);
                    top[std::get<function_stmt_t>(m_ast.get(function).node).name.text].push_back(
                        function);
                    index_declaration(function, std::format("{}.{}", name, text),
                                      { .module = module });
                }
            }
        }
    }

    /// The modules named `name`, or the files of the folder it names, as an Import finds them.
    [[nodiscard]] std::vector<module_id_t> modules_named(const std::string_view name) const {
        std::vector<module_id_t> found;
        const std::vector<module_t>& modules = m_ast.modules().modules();
        for (std::size_t i = 0; i < modules.size(); ++i) {
            if (modules[i].name == name) {
                return { static_cast<module_id_t>(i) };
            }
        }

        const std::string prefix = std::string(name) + ".";
        for (std::size_t i = 0; i < modules.size(); ++i) {
            const std::string& each = modules[i].name;
            if (each.starts_with(prefix) && !each.substr(prefix.size()).contains('.')) {
                found.push_back(static_cast<module_id_t>(i));
            }
        }

        return found;
    }

    [[nodiscard]] static const modifiers_t* modifiers_of(const stmt_node_t& node) {
        return std::visit(
            [](const auto& declared) -> const modifiers_t* {
                if constexpr (requires { declared.modifiers; }) {
                    return &declared.modifiers;
                } else {
                    return nullptr;
                }
            },
            node);
    }

    /// Whether code in `from` sees a top-level declaration of `module`.
    [[nodiscard]] bool visible(const stmt_id_t id, const module_id_t module,
                               const module_id_t from) const {
        if (module == from) {
            return true;
        }

        const modifiers_t* modifiers = modifiers_of(m_ast.get(id).node);
        const visibility_t visibility =
            modifiers != nullptr ? modifiers->visibility : visibility_t::PRIVATE;
        const module_table_t& table = m_ast.modules();
        return visibility == visibility_t::PUBLIC ||
               (visibility == visibility_t::PACKAGE &&
                table.get(module).package == table.get(from).package);
    }

    /// A declaration named `name` among `modules`' top-level ones that code in `from` sees.
    [[nodiscard]] std::optional<found_t> in_modules(const std::span<const module_id_t> modules,
                                                    const std::string_view name,
                                                    const module_id_t from) const {
        for (const module_id_t module : modules) {
            const auto& top = m_top[std::to_underlying(module)];
            const auto found = top.find(name);
            if (found == top.end()) {
                continue;
            }

            for (const stmt_id_t id : found->second) {
                if (visible(id, module, from)) {
                    return declaration(id);
                }
            }
        }

        return std::nullopt;
    }

    [[nodiscard]] found_t declaration(const stmt_id_t id) const {
        return { .path = path(id), .declaration = id };
    }

    /// A name as code in `scope` finds it: its type's members, its module's declarations, those
    /// of the modules it imports, and the built-ins.
    [[nodiscard]] std::optional<found_t> in_scope(const std::string_view name,
                                                  const scope_t& scope) const {
        if (scope.type) {
            if (std::optional<found_t> found =
                    member(declaration(*scope.type), name, scope.module)) {
                return found;
            }
        }

        if (scope.extension) {
            if (std::optional<found_t> found = own_member(*scope.extension, name)) {
                return found;
            }

            if (const std::optional<found_t> type = extended(*scope.extension)) {
                if (std::optional<found_t> found = member(*type, name, scope.module)) {
                    return found;
                }
            }
        }

        const module_id_t module = scope.module;
        if (std::optional<found_t> found = in_modules(std::span(&module, 1), name, module)) {
            return found;
        }

        for (const module_import_t& imported : m_ast.modules().get(module).imports) {
            const auto& node = std::get<import_stmt_t>(m_ast.get(imported.statement).node);
            if (node.alias) {
                if (node.alias->text == name) {
                    return found_t{ .path = std::string(name), .modules = imported.modules };
                }

                continue;
            }

            if (node.from) {
                if (std::ranges::contains(node.names, name, &name_t::text)) {
                    if (std::optional<found_t> found = in_modules(imported.modules, name, module)) {
                        return found;
                    }
                }

                continue;
            }

            if (node.last.text == name) {
                return found_t{ .path = node.target, .modules = imported.modules };
            }

            if (std::optional<found_t> found = in_modules(imported.modules, name, module)) {
                return found;
            }
        }

        if (builtin_type(name) || m_builtins.contains(name)) {
            return found_t{ .path = std::string(name), .builtin = true };
        }

        return std::nullopt;
    }

    /// A member that a type or Extend declares itself, or an Enum's.
    [[nodiscard]] std::optional<found_t> own_member(const stmt_id_t id,
                                                    const std::string_view name) const {
        const stmt_node_t& node = m_ast.get(id).node;
        const block_t* members = nullptr;
        if (const auto* type = std::get_if<type_stmt_t>(&node)) {
            members = &type->members;
        } else if (const auto* extension = std::get_if<extend_stmt_t>(&node)) {
            members = &extension->members;
        } else if (const auto* declared = std::get_if<enum_stmt_t>(&node)) {
            if (std::ranges::contains(declared->members, name, [](const enum_member_t& member) {
                    return member.name.text;
                })) {
                return found_t{ .path = std::format("{}.{}", path(id), name) };
            }

            return std::nullopt;
        }

        if (members == nullptr) {
            return std::nullopt;
        }

        for (const stmt_id_t member : *members) {
            const std::optional<name_t> declared = declared_name(m_ast.get(member).node);
            const bool new_named = name == "New" && declared && declared->text == "New";
            if ((declared && declared->text == name) || new_named) {
                return declaration(member);
            }
        }

        return std::nullopt;
    }

    /// A member named `name` of what `found` is, as code in `from` finds it: a module's
    /// declaration, or a type's own member, one an Extend that `from` sees adds, or one it has
    /// from the Class it extends or the Interfaces it implements.
    [[nodiscard]] std::optional<found_t> member(const found_t& found, const std::string_view name,
                                                const module_id_t from,
                                                const std::uint32_t depth = 0) const {
        if (!found.modules.empty()) {
            return in_modules(found.modules, name, from);
        }

        if (depth > 32) {
            return std::nullopt;
        }

        if (found.declaration) {
            if (std::optional<found_t> own = own_member(*found.declaration, name)) {
                return own;
            }
        }

        if (std::optional<found_t> added = extension_member(found, name, from)) {
            return added;
        }

        if (!found.declaration) {
            return std::nullopt;
        }

        const stmt_id_t declaration = *found.declaration;
        const auto* type = std::get_if<type_stmt_t>(&m_ast.get(declaration).node);
        const std::optional<scope_t> where = scope(declaration);
        if (type == nullptr || !where) {
            return std::nullopt;
        }

        // What it inherits, each found where it's declared.
        std::vector<std::optional<found_t>> bases;
        if (type->base) {
            std::optional<found_t> base =
                type->base_qualifier
                    ? in_scope(type->base_qualifier->text, { .module = where->module })
                    : in_scope(type->base->text, { .module = where->module });
            if (base && type->base_qualifier) {
                base = member(*base, type->base->text, where->module, depth + 1);
            }

            bases.push_back(std::move(base));
        }

        for (const type_expr_id_t interface : type->interfaces) {
            if (const auto* named = std::get_if<named_type_t>(&m_ast.get(interface).node)) {
                bases.push_back(in_scope(named->name, { .module = where->module }));
            }
        }

        for (const std::optional<found_t>& base : bases) {
            if (base && base->declaration != declaration) {
                if (std::optional<found_t> inherited = member(*base, name, from, depth + 1)) {
                    return inherited;
                }
            }
        }

        return std::nullopt;
    }

    /// A member an Extend of what `found` is adds, among the modules `from` sees: its own, and
    /// those it imports.
    [[nodiscard]] std::optional<found_t> extension_member(const found_t& found,
                                                          const std::string_view name,
                                                          const module_id_t from) const {
        std::vector<module_id_t> seen{ from };
        for (const module_import_t& imported : m_ast.modules().get(from).imports) {
            std::ranges::copy(imported.modules, std::back_inserter(seen));
        }

        for (const module_id_t module : seen) {
            for (const stmt_id_t id : items(module)) {
                if (!std::holds_alternative<extend_stmt_t>(m_ast.get(id).node)) {
                    continue;
                }

                const std::optional<found_t> type = extended(id);
                const bool same =
                    type &&
                    (found.builtin ? type->builtin && type->path == found.path
                                   : type->declaration && type->declaration == found.declaration);
                if (same) {
                    if (std::optional<found_t> added = own_member(id, name)) {
                        return added;
                    }
                }
            }
        }

        return std::nullopt;
    }
};

/// The text of the tokens from `begin` up to `end`, one space where a line break or space was
/// between them, but for one after an opening bracket or before a closing one.
[[nodiscard]] std::string tokens_text(const source_file_t& file,
                                      const std::span<const token_t> tokens,
                                      const std::uint32_t begin, const std::uint32_t end) {
    std::string text;
    const token_t* previous = nullptr;
    const auto first = std::ranges::lower_bound(
        tokens, begin, {}, [](const token_t& token) { return token.span.begin; });
    for (auto at = first; at != tokens.end() && at->span.end <= end; ++at) {
        if (at->kind == token_kind_t::NEWLINE || at->kind == token_kind_t::END_OF_FILE) {
            continue;
        }

        if (previous != nullptr && previous->span.end < at->span.begin) {
            const std::string_view gap =
                file.text().substr(previous->span.end, at->span.begin - previous->span.end);
            const bool broken = gap.find_first_of("\r\n") != std::string_view::npos;
            const bool opening = previous->kind == token_kind_t::LEFT_PAREN ||
                                 previous->kind == token_kind_t::LEFT_BRACKET ||
                                 previous->kind == token_kind_t::LEFT_BRACE;
            const bool closing = at->kind == token_kind_t::RIGHT_PAREN ||
                                 at->kind == token_kind_t::RIGHT_BRACKET ||
                                 at->kind == token_kind_t::RIGHT_BRACE;
            if (!broken || (!opening && !closing)) {
                text += ' ';
            }
        }

        text += file.slice(at->span);
        previous = &*at;
    }

    return text;
}

/// Makes fb doc's items for the package's modules.
class documenter_t {
public:
    documenter_t(const compilation_t& compilation, const build_mode_t mode,
                 diagnostic_bag_t& diagnostics)
        : m_compilation(compilation), m_ast(compilation.ast), m_index(compilation),
          m_defined(defined_symbols(mode)), m_diagnostics(diagnostics) {
        const analysis_t& analysis = compilation.analysis;
        std::unordered_map<type_id_t, symbol_id_t> types;
        for (std::size_t i = 0; i < analysis.symbols.size(); ++i) {
            const symbol_t& symbol = analysis.symbols[i];
            if (symbol.kind == symbol_kind_t::TYPE) {
                types.emplace(symbol.type, static_cast<symbol_id_t>(i));
            }
        }

        // What implements each Interface's member, to give the doc of one that has none.
        for (const conformance_t& conformance : analysis.conformances) {
            type_id_t interface = conformance.interface;
            if (conformance.interface == type_id_t::INVALID) {
                continue;
            }

            if (const type_id_t generic = analysis.types.get(interface).generic;
                generic != type_id_t::INVALID) {
                interface = generic;
            }

            const auto found = types.find(interface);
            if (found == types.end()) {
                continue;
            }

            const std::vector<symbol_id_t>& required = analysis.symbol(found->second).members;
            for (std::size_t i = 0; i < conformance.members.size() && i < required.size(); ++i) {
                if (conformance.members[i] != required[i]) {
                    m_implements.emplace(conformance.members[i], required[i]);
                }
            }
        }
    }

    doc_package_t run(const bool standard) {
        doc_package_t package;
        const module_table_t& modules = m_ast.modules();
        const std::optional<module_id_t> main = modules.main();
        std::optional<package_id_t> own;
        for (std::size_t i = 0; i < modules.size(); ++i) {
            const module_t& module = modules.get(static_cast<module_id_t>(i));
            if (standard ? modules.is_standard(module.package) : main && module.is_main) {
                own = module.package;
                break;
            }
        }

        if (standard) {
            package.name = "std";
            package.version = std::string(build_config::version);
        } else if (m_compilation.package) {
            package.name = m_compilation.package->name;
            package.version = m_compilation.package->version;
        }

        for (std::size_t i = 0; own && i < modules.size(); ++i) {
            const auto id = static_cast<module_id_t>(i);
            const module_t& module = modules.get(id);
            if (module.package != *own) {
                continue;
            }

            check_comments(module.file);
            doc_module_t documented = document_module(id);
            if (!documented.items.empty() || documented.doc) {
                package.modules.push_back(std::move(documented));
            }
        }

        std::ranges::sort(package.modules, {}, &doc_module_t::name);
        return package;
    }

private:
    const compilation_t& m_compilation;
    const ast_t& m_ast;
    api_index_t m_index;
    std::vector<std::string_view> m_defined;
    diagnostic_bag_t& m_diagnostics;
    /// Each file's tokens, for signatures.
    std::unordered_map<file_id_t, std::vector<token_t>> m_tokens;
    std::unordered_map<symbol_id_t, symbol_id_t> m_implements;

    [[nodiscard]] const source_file_t& file_of(const span_t span) const {
        return m_compilation.sources.get(span.file);
    }

    const std::vector<token_t>& tokens(const file_id_t file) {
        auto found = m_tokens.find(file);
        if (found == m_tokens.end()) {
            diagnostic_bag_t ignored;
            found = m_tokens.emplace(file, lex(m_compilation.sources.get(file), ignored, m_defined))
                        .first;
        }

        return found->second;
    }

    /// A file's path from its package's folder.
    [[nodiscard]] static std::string package_path(const module_t& module) {
        const std::size_t slash = module.path.find('/');
        return slash == std::string::npos ? module.path : module.path.substr(slash + 1);
    }

    /// The warnings about a file's doc comments that are on no declaration.
    void check_comments(const file_id_t file) {
        std::vector<span_t> comments;
        diagnostic_bag_t ignored;
        const source_file_t& source = m_compilation.sources.get(file);
        static_cast<void>(lex(source, ignored, m_defined, &comments));
        std::unordered_set<std::uint32_t> attached;
        for (const doc_comment_t* doc : m_ast.docs_in(file)) {
            for (const span_t line : doc->lines) {
                attached.insert(source.location_of(line.begin, column_unit_t::UTF8).line);
            }
        }

        const doc_comment_t* module_doc = m_ast.module_doc(file);
        for (const span_t comment : comments) {
            const std::uint32_t line = source.location_of(comment.begin, column_unit_t::UTF8).line;
            const bool on_module =
                module_doc != nullptr &&
                std::ranges::any_of(module_doc->lines, [&](const span_t each) {
                    return source.location_of(each.begin, column_unit_t::UTF8).line == line;
                });
            if (attached.contains(line) || on_module) {
                continue;
            }

            if (source.slice(comment).starts_with("//!")) {
                m_diagnostics.warning(comment,
                                      "a //! comment documents its module, so it goes at "
                                      "the top of its file, before its first line of code");
            } else {
                m_diagnostics.warning(comment,
                                      "this doc comment documents nothing: a /// comment goes on "
                                      "the lines right before a declaration");
            }
        }
    }

    /// What each of a doc's links names, as code in `scope` finds it, with a warning for each
    /// that names nothing when `warn`.
    std::vector<doc_target_t> links_of(const doc_t& doc, const scope_t& scope, const bool warn) {
        std::vector<doc_target_t> targets;
        for (const doc_link_t& link : doc.links) {
            const std::optional<found_t> found = m_index.resolve(link.target, scope);
            if (!found && warn) {
                m_diagnostics.warning(link.span, "'{}' names nothing that code here sees",
                                      link.target);
            }

            targets.push_back({
                .text = link.target,
                .path = found ? found->path : std::string(),
                .builtin = found && found->builtin,
            });
        }

        return targets;
    }

    doc_module_t document_module(const module_id_t id) {
        const module_t& module = m_ast.modules().get(id);
        doc_module_t documented{ .name = module.name, .file = package_path(module) };
        if (const doc_comment_t* comment = m_ast.module_doc(module.file)) {
            documented.doc = read_doc(*comment);
            documented.links = links_of(*documented.doc, { .module = id }, true);
        }

        for (const stmt_id_t item : m_index.items(id)) {
            const stmt_node_t& node = m_ast.get(item).node;
            if (const auto* block = std::get_if<extern_stmt_t>(&node)) {
                for (const stmt_id_t function : block->functions) {
                    add_item(documented.items, function, visibility_t::PUBLIC);
                }
            } else if (std::holds_alternative<extend_stmt_t>(node)) {
                add_extension(documented.items, item);
            } else {
                add_item(documented.items, item, visibility_t::PUBLIC);
            }
        }

        return documented;
    }

    [[nodiscard]] static const modifiers_t* modifiers_of(const stmt_node_t& node) {
        return std::visit(
            [](const auto& declared) -> const modifiers_t* {
                if constexpr (requires { declared.modifiers; }) {
                    return &declared.modifiers;
                } else {
                    return nullptr;
                }
            },
            node);
    }

    /// The kind fb doc names a declaration by.
    [[nodiscard]] static std::string kind_of(const stmt_node_t& node, const bool member) {
        if (const auto* function = std::get_if<function_stmt_t>(&node)) {
            switch (function->kind) {
            case function_kind_t::FUNCTION:
                return "function";
            case function_kind_t::METHOD:
            case function_kind_t::BYREF_METHOD:
                return function->modifiers.is_static ? "static method" : "method";
            case function_kind_t::CONSTRUCTOR:
                return "constructor";
            case function_kind_t::OPERATOR:
                return "operator";
            }
        }

        if (const auto* type = std::get_if<type_stmt_t>(&node)) {
            if (type->is_attribute) {
                return "attribute";
            }

            if (type->is_interface) {
                return "interface";
            }

            return type->is_class ? "class" : "struct";
        }

        if (const auto* variable = std::get_if<variable_stmt_t>(&node)) {
            switch (variable->kind) {
            case variable_kind_t::GLOBAL:
                return "global";
            case variable_kind_t::CONSTANT:
                return "const";
            case variable_kind_t::FIELD:
            case variable_kind_t::LOCAL:
                return variable->modifiers.is_static || !member ? "static field" : "field";
            }
        }

        if (std::holds_alternative<enum_stmt_t>(node)) {
            return "enum";
        }

        if (std::holds_alternative<property_stmt_t>(node)) {
            return "property";
        }

        return "alias";
    }

    /// Where a declaration's first line starts, at its first attribute or modifier, and where
    /// what fb doc shows of it ends.
    [[nodiscard]] std::pair<std::uint32_t, std::uint32_t> extent(const stmt_id_t id) const {
        const stmt_t& stmt = m_ast.get(id);
        const modifiers_t* modifiers = modifiers_of(stmt.node);
        const std::uint32_t start =
            modifiers != nullptr && modifiers->start ? *modifiers->start : stmt.span.begin;
        std::uint32_t end = stmt.span.end;
        if (const auto* function = std::get_if<function_stmt_t>(&stmt.node)) {
            end = function->header_end;
        } else if (const auto* type = std::get_if<type_stmt_t>(&stmt.node)) {
            // An Attribute is its one line.
            end = type->is_attribute ? stmt.span.end : type->header_end;
        } else if (const auto* property = std::get_if<property_stmt_t>(&stmt.node)) {
            end = property->header_end;
        } else if (const auto* extension = std::get_if<extend_stmt_t>(&stmt.node)) {
            end = extension->header_end;
        } else if (const auto* declared = std::get_if<enum_stmt_t>(&stmt.node)) {
            end = declared->type ? m_ast.get(*declared->type).span.end : declared->name.span.end;
        } else if (const auto* variable = std::get_if<variable_stmt_t>(&stmt.node)) {
            if (variable->kind != variable_kind_t::CONSTANT && variable->type) {
                end = m_ast.get(*variable->type).span.end;
            }
        }

        return { std::min(start, stmt.span.begin), end };
    }

    /// Fills in an item's signature, attributes, and place from its first line.
    void describe(doc_item_t& item, const span_t written, const std::uint32_t begin,
                  const std::uint32_t end) {
        const source_file_t& file = file_of(written);
        const std::vector<token_t>& all = tokens(written.file);
        std::uint32_t signature = begin;
        auto at = std::ranges::lower_bound(all, begin, {},
                                           [](const token_t& token) { return token.span.begin; });
        // Each attribute: '@', its name, which may be qualified, and its arguments.
        while (at != all.end() && at->kind == token_kind_t::AT && at->span.end <= end) {
            const std::uint32_t first = at->span.begin;
            ++at;
            while (at != all.end() &&
                   (at->kind == token_kind_t::IDENTIFIER || at->kind == token_kind_t::DOT)) {
                ++at;
            }

            if (at != all.end() && at->kind == token_kind_t::LEFT_PAREN) {
                std::uint32_t depth = 0;
                do {
                    if (at->kind == token_kind_t::LEFT_PAREN) {
                        ++depth;
                    } else if (at->kind == token_kind_t::RIGHT_PAREN) {
                        --depth;
                    }

                    ++at;
                } while (at != all.end() && depth > 0);
            }

            const std::uint32_t last = std::prev(at)->span.end;
            item.attributes.push_back(tokens_text(file, all, first, last));
            while (at != all.end() && at->kind == token_kind_t::NEWLINE) {
                ++at;
            }

            signature = at != all.end() ? at->span.begin : end;
        }

        item.signature = tokens_text(file, all, signature, end);
        item.line = file.location_of(begin, column_unit_t::UTF8).line + 1;
        const std::optional<module_id_t> module = m_ast.modules().of(written.file);
        item.file = module ? package_path(m_ast.modules().get(*module)) : file.path();
    }

    /// The doc an item has: its own, or for a member without one, that of what it implements or
    /// overrides, whose links are found where that one is.
    void add_doc(doc_item_t& item, const stmt_id_t id, const span_t name) {
        const std::optional<scope_t> scope = m_index.scope(id);
        if (const doc_comment_t* comment = m_ast.doc(name)) {
            item.doc = read_doc(*comment);
            item.links = links_of(*item.doc, scope.value_or(scope_t{}), true);
            return;
        }

        const analysis_t& analysis = m_compilation.analysis;
        std::optional<symbol_id_t> symbol = analysis.stmt(id).symbol;
        for (std::uint32_t depth = 0; symbol && depth < 32; ++depth) {
            const symbol_t& each = analysis.symbol(*symbol);
            const auto implemented = m_implements.find(*symbol);
            if (each.overrides) {
                symbol = each.overrides;
            } else if (implemented != m_implements.end()) {
                symbol = implemented->second;
            } else {
                symbol.reset();
            }

            if (!symbol) {
                break;
            }

            const symbol_t& next = analysis.symbol(*symbol);
            const doc_comment_t* comment = m_ast.doc(next.span);
            if (comment == nullptr || !next.declaration) {
                continue;
            }

            item.doc = read_doc(*comment);
            item.doc_from = m_index.path(*next.declaration);
            if (const std::optional<scope_t> from = m_index.scope(*next.declaration)) {
                item.links = links_of(*item.doc, *from, false);
            }

            return;
        }
    }

    /// Whether a type's member is in fb doc's items: what code outside its package sees of it.
    [[nodiscard]] static bool shown(const stmt_node_t& member, const type_stmt_t& type) {
        if (type.is_interface) {
            return true;
        }

        const modifiers_t* modifiers = modifiers_of(member);
        const visibility_t visibility =
            modifiers != nullptr ? modifiers->visibility : visibility_t::PRIVATE;
        return visibility == visibility_t::PUBLIC ||
               (type.is_class && visibility == visibility_t::PROTECTED);
    }

    void add_item(std::vector<doc_item_t>& items, const stmt_id_t id, const visibility_t parent,
                  const bool member = false) {
        const stmt_node_t& node = m_ast.get(id).node;
        const std::optional<name_t> name = declared_name(node);
        const modifiers_t* modifiers = modifiers_of(node);
        if (!name || name->text.empty() || modifiers == nullptr) {
            return;
        }

        if (!member && modifiers->visibility != visibility_t::PUBLIC) {
            return;
        }

        // A member is its type's, unless it's Protected.
        visibility_t visibility = visibility_t::PUBLIC;
        if (member) {
            visibility =
                modifiers->visibility == visibility_t::PROTECTED ? visibility_t::PROTECTED : parent;
        }

        doc_item_t item{
            .kind = kind_of(node, member),
            .name = std::string(name->text),
            .path = m_index.path(id),
            .visibility = visibility,
        };
        const auto [begin, end] = extent(id);
        describe(item, m_ast.get(id).span, begin, end);
        add_doc(item, id, name->span);
        // An Attribute's Fields are its parameters, which its signature shows.
        if (const auto* type = std::get_if<type_stmt_t>(&node);
            type != nullptr && !type->is_attribute) {
            for (const stmt_id_t each : type->members) {
                if (shown(m_ast.get(each).node, *type)) {
                    add_item(item.members, each, item.visibility, true);
                }
            }
        } else if (const auto* declared = std::get_if<enum_stmt_t>(&node)) {
            for (const enum_member_t& each : declared->members) {
                add_enum_member(item, id, each);
            }
        }

        items.push_back(std::move(item));
    }

    void add_enum_member(doc_item_t& owner, const stmt_id_t id, const enum_member_t& member) {
        doc_item_t item{
            .kind = "enum member",
            .name = std::string(member.name.text),
            .path = std::format("{}.{}", owner.path, member.name.text),
            .visibility = owner.visibility,
        };
        const std::uint32_t end =
            member.value ? m_ast.get(*member.value).span.end : member.name.span.end;
        describe(item, member.name.span, member.name.span.begin, end);
        if (const doc_comment_t* comment = m_ast.doc(member.name.span)) {
            item.doc = read_doc(*comment);
            item.links = links_of(*item.doc, m_index.scope(id).value_or(scope_t{}), true);
        }

        owner.members.push_back(std::move(item));
    }

    /// An Extend of what the package's users see: a built-in type, every T[], or a Public type,
    /// with its Public members.
    void add_extension(std::vector<doc_item_t>& items, const stmt_id_t id) {
        const auto& extension = std::get<extend_stmt_t>(m_ast.get(id).node);
        const std::optional<found_t> type = m_index.extended(id);
        if (!type) {
            return;
        }

        if (type->declaration) {
            const modifiers_t* modifiers = modifiers_of(m_ast.get(*type->declaration).node);
            if (modifiers == nullptr || modifiers->visibility != visibility_t::PUBLIC) {
                return;
            }
        }

        doc_item_t item{
            .kind = "extend",
            .name = m_index.extended_name(extension),
            .path = m_index.path(id),
            .extends = type->path,
        };
        const auto [begin, end] = extent(id);
        describe(item, m_ast.get(id).span, begin, end);
        for (const stmt_id_t member : extension.members) {
            const modifiers_t* modifiers = modifiers_of(m_ast.get(member).node);
            if (modifiers != nullptr && modifiers->visibility == visibility_t::PUBLIC) {
                add_item(item.members, member, visibility_t::PUBLIC, true);
            }
        }

        if (!item.members.empty() || !extension.interfaces.empty()) {
            items.push_back(std::move(item));
        }
    }
};

// JSON

/// `text` as a JSON string, in quotes.
[[nodiscard]] std::string quoted(const std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                out += std::format("\\u{:04x}", static_cast<unsigned>(c));
            } else {
                out += c;
            }
        }
    }

    return out + "\"";
}

/// Writes JSON with two spaces a level, each member on a line of its own.
class json_writer_t {
public:
    void open(const char bracket) {
        value_start();
        m_out += bracket;
        m_first.push_back(true);
    }

    void close(const char bracket) {
        const bool empty = m_first.back();
        m_first.pop_back();
        if (!empty) {
            newline();
        }

        m_out += bracket;
    }

    void key(const std::string_view name) {
        separate();
        m_out += quoted(name) + ": ";
        m_after_key = true;
    }

    void string(const std::string_view text) {
        value_start();
        m_out += quoted(text);
    }

    void number(const std::uint64_t value) {
        value_start();
        m_out += std::to_string(value);
    }

    void boolean(const bool value) {
        value_start();
        m_out += value ? "true" : "false";
    }

    void null() {
        value_start();
        m_out += "null";
    }

    std::string finish() {
        return std::move(m_out) + "\n";
    }

private:
    std::string m_out;
    std::vector<bool> m_first;
    bool m_after_key = false;

    void newline() {
        m_out += "\n" + std::string(m_first.size() * 2, ' ');
    }

    void separate() {
        if (m_first.empty()) {
            return;
        }

        if (!m_first.back()) {
            m_out += ',';
        }

        m_first.back() = false;
        newline();
    }

    void value_start() {
        if (m_after_key) {
            m_after_key = false;
            return;
        }

        separate();
    }
};

void write_links(json_writer_t& out, const std::vector<doc_target_t>& links) {
    out.key("links");
    out.open('[');
    for (const doc_target_t& link : links) {
        out.open('{');
        out.key("text");
        out.string(link.text);
        out.key("path");
        out.string(link.path);
        out.key("builtin");
        out.boolean(link.builtin);
        out.close('}');
    }

    out.close(']');
}

void write_doc(json_writer_t& out, const std::optional<doc_t>& doc,
               const std::vector<doc_target_t>& links, const source_manager_t& sources) {
    out.key("doc");
    if (!doc) {
        out.null();
        return;
    }

    out.open('{');
    out.key("summary");
    out.string(doc->summary);
    out.key("markdown");
    out.string(doc->markdown);
    out.key("sections");
    out.open('[');
    for (const doc_section_t& section : doc->sections) {
        out.open('{');
        out.key("heading");
        out.string(section.heading);
        out.key("markdown");
        out.string(section.markdown);
        out.close('}');
    }

    out.close(']');
    write_links(out, links);
    out.key("examples");
    out.open('[');
    for (const doc_example_t& example : doc->examples) {
        out.open('{');
        out.key("code");
        out.string(example.shown());
        out.key("line");
        out.number(sources.get(example.fence.file)
                       .location_of(example.fence.begin, column_unit_t::UTF8)
                       .line +
                   1);
        out.key("runs");
        out.boolean(example.kind == example_kind_t::RUN);
        out.close('}');
    }

    out.close(']');
    out.close('}');
}

void write_item(json_writer_t& out, const doc_item_t& item, const source_manager_t& sources) {
    out.open('{');
    out.key("kind");
    out.string(item.kind);
    out.key("name");
    out.string(item.name);
    out.key("path");
    out.string(item.path);
    out.key("signature");
    out.string(item.signature);
    out.key("attributes");
    out.open('[');
    for (const std::string& attribute : item.attributes) {
        out.string(attribute);
    }

    out.close(']');
    out.key("visibility");
    out.string(spelling(item.visibility));
    out.key("file");
    out.string(item.file);
    out.key("line");
    out.number(item.line);
    write_doc(out, item.doc, item.links, sources);
    if (!item.doc_from.empty()) {
        out.key("doc_from");
        out.string(item.doc_from);
    }

    if (!item.extends.empty()) {
        out.key("extends");
        out.string(item.extends);
    }

    if (!item.members.empty() || item.kind == "struct" || item.kind == "class" ||
        item.kind == "interface" || item.kind == "enum" || item.kind == "extend") {
        out.key("members");
        out.open('[');
        for (const doc_item_t& member : item.members) {
            write_item(out, member, sources);
        }

        out.close(']');
    }

    out.close('}');
}

} // namespace

doc_package_t document(const compilation_t& compilation, const bool standard,
                       const build_mode_t mode, diagnostic_bag_t& diagnostics) {
    // The warnings go in source order.
    diagnostic_bag_t found;
    doc_package_t package = documenter_t(compilation, mode, found).run(standard);
    found.sort();
    for (const diagnostic_t& diagnostic : found.diagnostics()) {
        diagnostics.report(diagnostic);
    }

    return package;
}

std::string doc_json(const doc_package_t& package, const source_manager_t& sources) {
    json_writer_t out;
    out.open('{');
    out.key("format");
    out.number(1);
    out.key("package");
    out.string(package.name);
    out.key("version");
    out.string(package.version);
    out.key("modules");
    out.open('[');
    for (const doc_module_t& module : package.modules) {
        out.open('{');
        out.key("name");
        out.string(module.name);
        out.key("file");
        out.string(module.file);
        write_doc(out, module.doc, module.links, sources);
        out.key("items");
        out.open('[');
        for (const doc_item_t& item : module.items) {
            write_item(out, item, sources);
        }

        out.close(']');
        out.close('}');
    }

    out.close(']');
    out.close('}');
    return out.finish();
}

} // namespace fb
