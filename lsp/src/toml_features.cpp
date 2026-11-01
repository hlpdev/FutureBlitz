#include "fb/lsp/toml_features.hpp"

#include "fb/driver.hpp"
#include "fb/lsp/features.hpp"
#include "fb/lsp/protocol.hpp"
#include "fb/manifest.hpp"
#include "fb/packages.hpp"
#include "fb/registry.hpp"
#include "fb/registry_api.hpp"
#include "fb/semver.hpp"
#include "fb/source.hpp"
#include "fb/utf8.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace fb::lsp {
namespace {

using steady_t = std::chrono::steady_clock;

/// The LSP's CompletionItemKind for what each item is.
constexpr int module_kind = 9;
constexpr int property_kind = 10;
constexpr int value_kind = 12;
constexpr int enum_member_kind = 20;

/// How many packages a name's completion asks a registry for.
constexpr std::size_t searched_packages = 20;

bool is_key_char(const char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-';
}

std::string_view trimmed(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }

    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }

    return text;
}

/// A key as TOML writes it, bare or quoted, without its quotes.
std::string unquoted(std::string_view key) {
    key = trimmed(key);
    if (key.size() >= 2 && (key.front() == '"' || key.front() == '\'') &&
        key.back() == key.front()) {
        key = key.substr(1, key.size() - 2);
    }

    return std::string(key);
}

/// The line of `text` that `offset` is on: where it starts, and where it ends, before its break,
/// "\n" or "\r\n".
std::pair<std::uint32_t, std::uint32_t> line_at(const std::string_view text,
                                                const std::uint32_t offset) {
    const std::size_t before = offset == 0 ? std::string_view::npos : text.rfind('\n', offset - 1);
    const std::size_t begin = before == std::string_view::npos ? 0 : before + 1;
    std::size_t end = std::min(text.find('\n', offset), text.size());
    if (end > begin && text[end - 1] == '\r') {
        --end;
    }

    return { static_cast<std::uint32_t>(begin), static_cast<std::uint32_t>(end) };
}

/// The line break `text` has, "\r\n" or "\n", which what's put in it has too.
std::string_view line_break(const std::string_view text) {
    return text.contains("\r\n") ? "\r\n" : "\n";
}

/// A table's header: `[dependencies]`, or `[dependencies.physics]`, a dependency's own table.
struct header_t {
    std::string table;
    std::string sub;
    /// Where the header's line is.
    std::uint32_t line = 0;
};

/// The header of a line that's one, as it's typed, before its `]` too.
std::optional<header_t> header_of(const std::string_view line) {
    std::string_view text = trimmed(line);
    if (!text.starts_with('[') || text.starts_with("[[")) {
        return std::nullopt;
    }

    text.remove_prefix(1);
    text = text.substr(0, text.find(']'));
    const std::size_t dot = text.find('.');
    return header_t{
        .table = unquoted(text.substr(0, dot)),
        .sub = dot == std::string_view::npos ? std::string() : unquoted(text.substr(dot + 1)),
    };
}

/// Each line of `text`, from the start, as offsets: a value written over several lines, a
/// multi-line string, is one, from its key's line to the end of the line that closes it.
std::vector<std::pair<std::uint32_t, std::uint32_t>> lines_of(const std::string_view text) {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> lines;
    std::string_view open;
    std::uint32_t first = 0;
    std::uint32_t last = 0;
    for (std::size_t at = 0; at <= text.size();) {
        const auto [begin, end] = line_at(text, static_cast<std::uint32_t>(at));
        const std::string_view line = text.substr(begin, end - begin);
        if (open.empty()) {
            first = begin;
        }

        // Three quotes, of either kind, open a multi-line string, or close the one that's open; a
        // string on one line is passed over, and a comment ends what's read of the line.
        for (std::size_t found = 0; found < line.size(); ++found) {
            const std::string_view rest = line.substr(found);
            if (open.empty() && (rest.starts_with(R"(""")") || rest.starts_with(R"(''')"))) {
                open = rest.substr(0, 3);
                found += 2;
            } else if (!open.empty() && rest.starts_with(open)) {
                open = {};
                found += 2;
            } else if (open.empty() && (rest.front() == '"' || rest.front() == '\'')) {
                const std::size_t close = line.find(rest.front(), found + 1);
                found = close == std::string_view::npos ? line.size() : close;
            } else if (open.empty() && rest.front() == '#') {
                break;
            }
        }

        last = end;
        if (open.empty()) {
            lines.emplace_back(first, end);
        }

        const std::size_t next = text.find('\n', begin);
        if (next == std::string_view::npos) {
            break;
        }

        at = next + 1;
    }

    // A string that's never closed goes to the end.
    if (!open.empty()) {
        lines.emplace_back(first, last);
    }

    return lines;
}

/// The header of the table `offset` is in, or nullopt before the first.
std::optional<header_t> table_at(const std::string_view text, const std::uint32_t offset) {
    std::optional<header_t> found;
    for (const auto& [begin, end] : lines_of(text)) {
        if (begin > offset) {
            break;
        }

        if (std::optional<header_t> header = header_of(text.substr(begin, end - begin));
            header && end < offset) {
            header->line = begin;
            found = std::move(header);
        }
    }

    return found;
}

/// The key of a line, `name = ...`, or nullopt for one that has none, as a header or a comment.
std::optional<std::string> key_of(const std::string_view line) {
    const std::string_view text = trimmed(line);
    const std::size_t equals = text.find('=');
    if (text.empty() || text.starts_with('#') || text.starts_with('[') ||
        equals == std::string_view::npos) {
        return std::nullopt;
    }

    const std::string key = unquoted(text.substr(0, equals));
    return key.substr(0, key.find('.'));
}

/// The keys the table `table` has, `sub` of it for a dependency's own table, and where its last
/// line ends.
std::pair<std::set<std::string>, std::uint32_t> keys_in(const std::string_view text,
                                                        const std::string_view table,
                                                        const std::string_view sub = {}) {
    std::set<std::string> keys;
    std::uint32_t last = 0;
    bool inside = false;
    for (const auto& [begin, end] : lines_of(text)) {
        const std::string_view line = text.substr(begin, end - begin);
        if (const std::optional<header_t> header = header_of(line)) {
            inside = header->table == table && header->sub == sub;
            if (inside) {
                last = end;
            }

            continue;
        }

        if (inside) {
            if (std::optional<std::string> key = key_of(line)) {
                keys.insert(std::move(*key));
                last = end;
            }
        }
    }

    return { keys, last };
}

/// Where the cursor is on its line, as the features read a line that's being typed.
struct place_t {
    /// The line's key, before its `=`.
    std::string key;
    /// After the line's `=`.
    bool value = false;
    /// In the value's `{ ... }`: the key there, and whether after its `=`.
    bool in_table = false;
    std::string table_key;
    bool table_value = false;
    /// In a string: where its text starts.
    std::optional<std::uint32_t> string_begin;
    /// The word the cursor is at the end of, and where it starts.
    std::string word;
    std::uint32_t word_begin = 0;
    /// In a comment, where nothing's completed.
    bool comment = false;
};

/// Where the cursor at `offset` is, from the start of its line.
place_t place_at(const std::string_view text, const std::uint32_t offset) {
    const auto [begin, end] = line_at(text, offset);
    place_t place{ .word_begin = begin };
    char quote = 0;
    int depth = 0;
    std::string token;
    for (std::uint32_t at = begin; at < offset; ++at) {
        const char c = text[at];
        if (quote != 0) {
            if (c == '\\' && quote == '"') {
                ++at;
            } else if (c == quote) {
                quote = 0;
                place.string_begin.reset();
            } else {
                token += c;
            }

            continue;
        }

        if (c == '#') {
            place.comment = true;
            return place;
        }

        if (c == '"' || c == '\'') {
            quote = c;
            place.string_begin = at + 1;
            token.clear();
            continue;
        }

        if (is_key_char(c) || c == '.') {
            if (token.empty() || !is_key_char(text[at - 1])) {
                token.clear();
                place.word_begin = at;
            }

            token += c;
        } else {
            place.word_begin = at + 1;
        }

        if (c == '=' && depth == 0 && !place.value) {
            place.key = unquoted(std::string_view(text).substr(begin, at - begin));
            place.key = place.key.substr(0, place.key.find('.'));
            place.value = true;
            token.clear();
        } else if (c == '=' && depth == 1 && !place.table_value) {
            place.table_key =
                unquoted(token.empty() ? std::string_view() : std::string_view(token));
            place.table_value = true;
            token.clear();
        } else if (c == '{') {
            ++depth;
            place.in_table = depth == 1;
            place.table_key.clear();
            place.table_value = false;
            token.clear();
        } else if (c == ',' && depth == 1) {
            place.table_key.clear();
            place.table_value = false;
            token.clear();
        } else if (c == '}') {
            depth = std::max(0, depth - 1);
            place.in_table = depth > 0;
        }
    }

    // In a string, the word is its text so far.
    if (quote != 0) {
        place.word = token;
        place.word_begin = place.string_begin.value_or(place.word_begin);
    } else {
        place.word = std::string(text.substr(place.word_begin, offset - place.word_begin));
    }

    if (!place.value) {
        place.key = place.word;
    }

    return place;
}

/// The registries [registries] names, as it's typed: `name = { index = "..." }`.
std::vector<registry_entry_t> registries_in(const std::string_view text) {
    std::vector<registry_entry_t> registries;
    bool inside = false;
    for (const auto& [begin, end] : lines_of(text)) {
        const std::string_view line = text.substr(begin, end - begin);
        if (const std::optional<header_t> header = header_of(line)) {
            inside = header->table == "registries";
            continue;
        }

        const std::optional<std::string> name = key_of(line);
        const std::size_t index = line.find("index");
        if (!inside || !name || index == std::string_view::npos) {
            continue;
        }

        const std::size_t open = line.find('"', index);
        const std::size_t close = open == std::string_view::npos ? open : line.find('"', open + 1);
        if (close != std::string_view::npos) {
            registries.push_back(
                { .name = *name, .index = std::string(line.substr(open + 1, close - open - 1)) });
        }
    }

    return registries;
}

/// The value of the key `registry` on a line, `registry = "company"`, as it's typed, but not a
/// name that has the word in it, like `registry-tools`.
std::optional<std::string> registry_of(const std::string_view line) {
    constexpr std::string_view word = "registry";
    for (std::size_t key = line.find(word); key != std::string_view::npos;
         key = line.find(word, key + 1)) {
        const std::size_t after = line.find_first_not_of(" \t", key + word.size());
        if ((key > 0 && is_key_char(line[key - 1])) || after == std::string_view::npos ||
            line[after] != '=') {
            continue;
        }

        const std::size_t open = line.find('"', after);
        const std::size_t close = open == std::string_view::npos ? open : line.find('"', open + 1);
        if (close != std::string_view::npos) {
            return std::string(line.substr(open + 1, close - open - 1));
        }
    }

    return std::nullopt;
}

/// The registry a dependency names: on its line, `physics = { registry = "company" }`, or in its
/// own table, `[dependencies.physics]`, when `own` is its name.
std::optional<std::string> registry_named(const std::string_view text, const std::string_view line,
                                          const std::string_view own) {
    if (own.empty()) {
        return registry_of(line);
    }

    bool inside = false;
    for (const auto& [begin, end] : lines_of(text)) {
        const std::string_view each = text.substr(begin, end - begin);
        if (const std::optional<header_t> header = header_of(each)) {
            inside = header->table == "dependencies" && header->sub == own;
        } else if (inside && key_of(each) == "registry") {
            return registry_of(each);
        }
    }

    return std::nullopt;
}

/// The versions that aren't yanked, newest first.
std::vector<version_t> unyanked(const std::vector<index_entry_t>& entries) {
    std::vector<version_t> versions;
    for (const index_entry_t& entry : entries) {
        if (std::expected<version_t, std::string> version = parse_version(entry.version);
            version && !entry.yanked) {
            versions.push_back(std::move(*version));
        }
    }

    std::ranges::sort(versions, std::ranges::greater{});
    return versions;
}

/// A package's versions as the cache on disk has its index, read without the network.
std::vector<index_entry_t> cached_versions(const std::string& index, const std::string& name) {
    registry_t cache(index, send_http, true, {});
    return cache.versions(name).value_or(std::vector<index_entry_t>{});
}

/// The packages whose names start with `prefix` that the cache on disk has an index of, each
/// with its newest version that isn't yanked, sorted by name.
std::vector<found_package_t> cached_names(const std::string& index, const std::string& prefix) {
    std::vector<found_package_t> packages;
    const std::filesystem::path folder =
        fb_home() / "cache" / "index" / path_from_utf8(registry_folder(index));
    std::error_code error;
    for (std::filesystem::directory_iterator each(folder, error), end; !error && each != end;
         each.increment(error)) {
        const std::string name = to_utf8(each->path().filename());
        if (each->is_regular_file(error) && !name.contains('.') && name.starts_with(prefix)) {
            const std::vector<version_t> known = unyanked(cached_versions(index, name));
            packages.push_back(
                { .name = name, .version = known.empty() ? "" : known.front().text() });
        }
    }

    std::ranges::sort(packages, {}, &found_package_t::name);
    return packages;
}

/// A completion item that puts `text` where the word or string at the cursor is.
json_t item(const std::string& label, const int kind, const std::string& text,
            const json_t& range) {
    return {
        { "label", label },
        { "kind", kind },
        { "textEdit", { { "range", range }, { "newText", text } } },
    };
}

/// A text edit that puts `text` at `offset`.
json_t insertion(const source_file_t& file, const positions_t& positions,
                 const std::uint32_t offset, const std::string& text) {
    return {
        { "range", positions.range(file, { .file = file.id(), .begin = offset, .end = offset }) },
        { "newText", text },
    };
}

std::string markdown_key(const manifest_key_t& key) {
    std::string text = std::format("`{}`: {}.", key.key, key.description);
    if (!key.example.empty()) {
        text += std::format("\n\n```toml\n{} = {}\n```", key.key, key.example);
    }

    return text;
}

} // namespace

bool is_manifest(const std::filesystem::path& path) {
    return path.filename() == "project.toml";
}

toml_features_t::toml_features_t(send_t send, const positions_t& positions, options_t options)
    : m_send(std::move(send)), m_positions(positions), m_options(std::move(options)) {
    m_thread = std::thread([this] { run(); });
    m_fetcher = std::thread([this] { fetch(); });
}

toml_features_t::~toml_features_t() {
    {
        const std::scoped_lock lock(m_mutex);
        m_stopping = true;
    }

    m_work.notify_all();
    m_thread.join();
    {
        const std::scoped_lock lock(m_registry_mutex);
        m_fetcher_stopping = true;
    }

    m_fetch_work.notify_all();
    m_fetcher.join();
}

void toml_features_t::add_folders(const std::vector<std::filesystem::path>& folders) {
    const std::scoped_lock lock(m_mutex);
    std::ranges::copy(folders, std::back_inserter(m_folders));
}

void toml_features_t::remove_folders(const std::vector<std::filesystem::path>& folders) {
    const std::scoped_lock lock(m_mutex);
    std::erase_if(m_folders, [&](const std::filesystem::path& folder) {
        return std::ranges::any_of(folders, [&](const std::filesystem::path& removed) {
            return file_key(removed) == file_key(folder);
        });
    });
}

bool toml_features_t::owns(const std::filesystem::path& path) const {
    if (!is_manifest(path)) {
        return false;
    }

    const std::scoped_lock lock(m_mutex);
    return m_documents.contains(file_key(path));
}

std::optional<std::pair<std::string, std::int64_t>>
toml_features_t::document(const std::filesystem::path& path) const {
    const std::scoped_lock lock(m_mutex);
    const auto found = m_documents.find(file_key(path));
    if (found == m_documents.end()) {
        return std::nullopt;
    }

    return std::pair(found->second.text, found->second.version);
}

void toml_features_t::open(const std::filesystem::path& path, const std::int64_t version,
                           std::string text) {
    {
        const std::scoped_lock lock(m_mutex);
        const std::string key = file_key(path);
        m_documents[key] = { .path = path, .version = version, .text = std::move(text) };
        m_closed.erase(key);
        m_due[key] = steady_t::now();
    }

    m_work.notify_all();
}

void toml_features_t::change(const std::filesystem::path& path, const std::int64_t version,
                             std::string text) {
    {
        const std::scoped_lock lock(m_mutex);
        const std::string key = file_key(path);
        const auto found = m_documents.find(key);
        if (found == m_documents.end()) {
            return;
        }

        found->second.version = version;
        found->second.text = std::move(text);
        m_due[key] = steady_t::now() + m_options.delay;
    }

    m_work.notify_all();
}

void toml_features_t::close(const std::filesystem::path& path) {
    {
        const std::scoped_lock lock(m_mutex);
        const std::string key = file_key(path);
        if (m_documents.erase(key) == 0) {
            return;
        }

        m_closed[key] = path;
        m_due[key] = steady_t::now();
    }

    m_work.notify_all();
}

void toml_features_t::settle() {
    std::unique_lock lock(m_mutex);
    m_done.wait(lock, [this] { return m_due.empty() && !m_checking; });
}

void toml_features_t::run() {
    std::unique_lock lock(m_mutex);
    while (!m_stopping) {
        if (m_due.empty()) {
            m_work.wait(lock);
            continue;
        }

        const auto next =
            std::ranges::min_element(m_due, {}, [](const auto& due) { return due.second; });
        if (next->second > steady_t::now()) {
            m_work.wait_until(lock, next->second);
            continue;
        }

        const std::string key = next->first;
        m_due.erase(next);
        m_checking = true;
        lock.unlock();
        check(key);
        lock.lock();
        m_checking = false;
        m_done.notify_all();
    }
}

void toml_features_t::check(const std::string& key) {
    std::filesystem::path path;
    std::optional<std::int64_t> version;
    std::map<std::string, std::string> texts;
    std::vector<std::filesystem::path> folders;
    {
        const std::scoped_lock lock(m_mutex);
        if (const auto found = m_documents.find(key); found != m_documents.end()) {
            path = found->second.path;
            version = found->second.version;
        } else if (const auto closed = m_closed.find(key); closed != m_closed.end()) {
            path = closed->second;
            m_closed.erase(closed);
        } else {
            return;
        }

        for (const auto& [each, document] : m_documents) {
            texts[each] = document.text;
        }

        folders = m_folders;
    }

    // Each open project.toml is read as the editor has it, this one's and its dependencies'.
    const source_reader_t read =
        [&texts](const std::filesystem::path& file) -> std::optional<std::string> {
        const auto found = texts.find(file_key(file));
        return found != texts.end() ? std::optional(found->second) : std::nullopt;
    };
    std::shared_ptr<const compilation_t> checked;
    json_t diagnostics = json_t::array();
    std::expected<compilation_t, std::string> compiled =
        check_manifest(path.parent_path(), read, folders);
    if (compiled) {
        checked = std::make_shared<const compilation_t>(std::move(*compiled));
        const std::string wanted = file_key(path);
        for (const diagnostic_t& diagnostic : checked->diagnostics.diagnostics()) {
            const std::filesystem::path in = checked->disk_path(diagnostic.span.file);
            if (!in.empty() && file_key(in) == wanted) {
                diagnostics.push_back(diagnostic_json(*checked, diagnostic, m_positions));
            }
        }
    } else {
        diagnostics.push_back({
            {
                "range",
                {
                    { "start", { { "line", 0 }, { "character", 0 } } },
                    { "end", { { "line", 0 }, { "character", 0 } } },
                },
            },
            { "severity", 1 },
            { "source", "fb" },
            { "message", compiled.error() },
        });
    }

    json_t params{ { "uri", uri_of(path) }, { "diagnostics", std::move(diagnostics) } };
    {
        const std::scoped_lock lock(m_mutex);
        const auto found = m_documents.find(key);
        // A document edited since, or closed, or opened again, has a newer check coming.
        if (version ? found == m_documents.end() || found->second.version != *version ||
                          m_due.contains(key)
                    : found != m_documents.end()) {
            return;
        }

        if (version) {
            found->second.checked = std::move(checked);
            params["version"] = *version;
        }
    }

    m_send({
        { "jsonrpc", "2.0" },
        { "method", "textDocument/publishDiagnostics" },
        { "params", std::move(params) },
    });
}

std::optional<json_t> toml_features_t::answer(const std::string& method, const json_t& params) {
    if (!method.starts_with("textDocument/") || !params.contains("textDocument")) {
        return std::nullopt;
    }

    const std::optional<std::filesystem::path> path =
        path_of(params.at("textDocument").at("uri").get<std::string>());
    if (!path || !is_manifest(*path)) {
        return std::nullopt;
    }

    // What it doesn't do for a project.toml, as semantic tokens or formatting, it answers with
    // nothing, as the server would otherwise read the file as FutureBlitz code.
    if (method != "textDocument/hover" && method != "textDocument/completion" &&
        method != "textDocument/codeAction") {
        return json_t();
    }

    document_t document;
    {
        const std::scoped_lock lock(m_mutex);
        const auto found = m_documents.find(file_key(*path));
        if (found == m_documents.end()) {
            return json_t();
        }

        document = found->second;
    }

    if (method == "textDocument/hover") {
        return hover(document, params);
    }

    // A completion that's asking a registry is incomplete, so the client asks again.
    if (method == "textDocument/completion") {
        bool asking = false;
        json_t items = completion(document, params, asking);
        return json_t{ { "isIncomplete", asking }, { "items", std::move(items) } };
    }

    return code_actions(document, params);
}

std::vector<index_entry_t> toml_features_t::versions(const std::string& index,
                                                     const std::string& name, bool& asking) {
    const std::string key = index + '\n' + name;
    {
        const std::scoped_lock lock(m_registry_mutex);
        if (const auto found = m_versions.find(key); found != m_versions.end()) {
            if (steady_t::now() - found->second.when >= m_options.registry_life) {
                asking = true;
                ask({ .index = index, .what = name });
            }

            return found->second.value;
        }
    }

    // Until the registry says, what the cache on disk has, read without the network.
    std::vector<index_entry_t> cached = cached_versions(index, name);
    if (m_options.offline) {
        const std::scoped_lock lock(m_registry_mutex);
        m_versions[key] = { .value = cached, .when = steady_t::now() };
    } else {
        asking = true;
        const std::scoped_lock lock(m_registry_mutex);
        ask({ .index = index, .what = name });
    }

    return cached;
}

std::vector<found_package_t> toml_features_t::search(const std::string& index,
                                                     const std::string& prefix, bool& asking) {
    const std::string key = index + '\n' + prefix;
    {
        const std::scoped_lock lock(m_registry_mutex);
        if (const auto found = m_searches.find(key); found != m_searches.end()) {
            if (steady_t::now() - found->second.when >= m_options.registry_life) {
                asking = true;
                ask({ .search = true, .index = index, .what = prefix });
            }

            return found->second.value;
        }
    }

    std::vector<found_package_t> cached = cached_names(index, prefix);
    if (m_options.offline) {
        const std::scoped_lock lock(m_registry_mutex);
        m_searches[key] = { .value = cached, .when = steady_t::now() };
    } else {
        asking = true;
        const std::scoped_lock lock(m_registry_mutex);
        ask({ .search = true, .index = index, .what = prefix });
    }

    return cached;
}

void toml_features_t::ask(fetch_t fetch) {
    const std::string key = std::format("{}\n{}\n{}", fetch.search, fetch.index, fetch.what);
    if (m_fetching.insert(key).second) {
        m_fetches.push_back(std::move(fetch));
        m_fetch_work.notify_all();
    }
}

void toml_features_t::fetch() {
    std::unique_lock lock(m_registry_mutex);
    while (!m_fetcher_stopping) {
        if (m_fetches.empty()) {
            m_fetch_work.wait(lock);
            continue;
        }

        const fetch_t asked = std::move(m_fetches.front());
        m_fetches.pop_front();
        lock.unlock();
        // A registry_t remembers it couldn't reach its index, so one that couldn't is made again
        // once that's older than what's kept, as a search tries again.
        auto& [registry, failed] = m_registries[asked.index];
        if (registry == nullptr ||
            (failed && steady_t::now() - *failed >= m_options.registry_life)) {
            registry = std::make_unique<registry_t>(asked.index, m_options.http, m_options.offline,
                                                    status_t{});
            failed.reset();
        }

        std::vector<index_entry_t> entries;
        std::vector<found_package_t> packages;
        if (!asked.search) {
            // A registry out of reach gives the cache's, or nothing, as registry_t remembers it
            // is, until it's made again.
            std::expected<std::vector<index_entry_t>, std::string> asked_for =
                registry->versions(asked.what);
            if ((!asked_for || !registry->notes().empty()) && !failed) {
                failed = steady_t::now();
            }

            entries = std::move(asked_for).value_or(cached_versions(asked.index, asked.what));
        } else {
            const auto unreachable = m_unreachable.find(asked.index);
            const bool reachable = unreachable == m_unreachable.end() ||
                                   steady_t::now() - unreachable->second >= m_options.registry_life;
            const registry_session_t session{ .index = asked.index, .http = m_options.http };
            auto searched = reachable ? search_packages(asked.what, searched_packages, session)
                                      : std::unexpected(std::string("unreachable"));
            if (searched) {
                packages = std::move(searched->first);
                m_unreachable.erase(asked.index);
            } else {
                if (reachable) {
                    m_unreachable[asked.index] = steady_t::now();
                }

                packages = cached_names(asked.index, asked.what);
            }

            // A name that starts with what's typed comes first, the shortest first.
            std::ranges::stable_sort(packages, {}, [&](const found_package_t& package) {
                return std::pair(!package.name.starts_with(asked.what), package.name.size());
            });
        }

        lock.lock();
        const std::string key = asked.index + '\n' + asked.what;
        if (asked.search) {
            m_searches[key] = { .value = std::move(packages), .when = steady_t::now() };
        } else {
            m_versions[key] = { .value = std::move(entries), .when = steady_t::now() };
        }

        m_fetching.erase(std::format("{}\n{}\n{}", asked.search, asked.index, asked.what));
    }
}

json_t toml_features_t::completion(const document_t& document, const json_t& params, bool& asking) {
    const source_file_t file(file_id_t{}, to_utf8(document.path), document.text);
    const std::string_view text = file.text();
    const std::uint32_t offset = m_positions.offset(file, params.at("position"));
    const place_t place = place_at(text, offset);
    json_t items = json_t::array();
    if (place.comment) {
        return items;
    }

    const json_t range =
        m_positions.range(file, { .file = file.id(), .begin = place.word_begin, .end = offset });
    const auto [line_begin, line_end] = line_at(text, offset);
    const std::string_view line = text.substr(line_begin, line_end - line_begin);
    const std::optional<header_t> header = table_at(text, offset);
    const std::string table = header ? header->table : std::string();
    const std::filesystem::path root = document.path.parent_path();

    // A header, as `[` is typed: the tables.
    if (trimmed(line.substr(0, offset - line_begin)).starts_with('[')) {
        const bool closed = line.substr(offset - line_begin).contains(']');
        for (const manifest_table_t& each : manifest_tables()) {
            json_t entry = item(std::string(each.name), module_kind,
                                std::string(each.name) + (closed ? "" : "]"), range);
            entry["documentation"] = { { "kind", "markdown" }, { "value", each.description } };
            items.push_back(std::move(entry));
        }

        return items;
    }

    // A dependency's own table, `{ ... }` or `[dependencies.physics]`: its keys and their values.
    const bool own_table = header && !header->sub.empty();
    const std::string dependency = own_table ? header->sub : place.key;
    const bool in_keys = own_table ? !place.value : place.in_table && !place.table_value;
    const bool in_value = own_table ? place.value : place.in_table && place.table_value;
    const std::string field = own_table ? place.key : place.table_key;
    const std::string_view inner = table;
    const auto add_values = [&](const std::span<const std::string_view> values) {
        for (const std::string_view value : values) {
            items.push_back(item(
                std::string(value), enum_member_kind,
                place.string_begin ? std::string(value) : std::format("\"{}\"", value), range));
        }
    };
    const auto add_versions = [&](const std::string& name) {
        const std::vector<registry_entry_t> registries = registries_in(text);
        const std::optional<std::string> index = registry_index(
            registry_named(text, line, own_table ? header->sub : ""), registries, root);
        if (!index || !place.string_begin) {
            return;
        }

        std::size_t order = 0;
        for (const version_t& version : unyanked(versions(*index, name, asking))) {
            json_t entry = item(version.text(), value_kind, version.text(), range);
            entry["sortText"] = std::format("{:04}", order);
            entry["preselect"] = order++ == 0;
            items.push_back(std::move(entry));
        }
    };

    if ((place.in_table || own_table) && (in_keys || in_value)) {
        if (in_keys) {
            const std::set<std::string> written =
                own_table ? keys_in(text, table, header->sub).first : std::set<std::string>{};
            for (const manifest_key_t& key : manifest_keys()) {
                if (key.table == inner && !written.contains(std::string(key.key)) &&
                    !line.substr(0, offset - line_begin).contains(std::format("{} =", key.key))) {
                    json_t entry = item(std::string(key.key), property_kind,
                                        std::format("{} = ", key.key), range);
                    entry["documentation"] = {
                        { "kind", "markdown" },
                        { "value", markdown_key(key) },
                    };
                    items.push_back(std::move(entry));
                }
            }

            return items;
        }

        if (table == "dependencies" && field == "version") {
            add_versions(dependency);
        } else if (table == "dependencies" && field == "registry" && place.string_begin) {
            std::set<std::string> names{ std::string(default_registry) };
            for (const registry_entry_t& registry : registries_in(text)) {
                names.insert(registry.name);
            }

            for (const std::string& name : names) {
                items.push_back(item(name, value_kind, name, range));
            }
        }

        for (const manifest_key_t& key : manifest_keys()) {
            if (key.table == inner && key.key == field) {
                add_values(key.values);
            }
        }

        return items;
    }

    if (table == "dependencies") {
        // A dependency's range, `physics = "|"`: the registry's versions, newest first.
        if (place.value) {
            add_versions(place.key);
            return items;
        }

        // A dependency's name: the registry's packages, each with its newest version.
        const std::optional<std::string> index =
            registry_index(std::nullopt, registries_in(text), root);
        if (!index || place.word.empty()) {
            return items;
        }

        const std::set<std::string> written = keys_in(text, "dependencies").first;
        for (const found_package_t& package : search(*index, place.word, asking)) {
            if (written.contains(package.name) && package.name != place.word) {
                continue;
            }

            // A quoted key, `"phys`, is only the name; a bare one, the line.
            std::string line_text = std::format("{} = ", package.name);
            if (place.string_begin) {
                line_text = package.name;
            } else if (!package.version.empty()) {
                line_text = std::format("{} = \"{}\"", package.name, package.version);
            }

            json_t entry = item(package.name, module_kind, line_text, range);
            entry["detail"] = package.version;
            if (!package.description.empty()) {
                entry["documentation"] = package.description;
            }

            items.push_back(std::move(entry));
        }

        return items;
    }

    // [project]'s, [format]'s, and [build]'s keys, and the values of those that have few.
    if (place.value) {
        for (const manifest_key_t& key : manifest_keys()) {
            if (key.table == table && key.key == place.key) {
                add_values(key.values);
            }
        }

        if (table == "build" && place.key == "package" && place.string_begin) {
            for (const std::string& name : keys_in(text, "dependencies").first) {
                items.push_back(item(name, value_kind, name, range));
            }
        }

        return items;
    }

    const std::set<std::string> written = keys_in(text, table).first;
    for (const manifest_key_t& key : manifest_keys()) {
        if (key.table == table && table != "dependencies" && table != "native" &&
            table != "registries" && !written.contains(std::string(key.key))) {
            json_t entry =
                item(std::string(key.key), property_kind, std::format("{} = ", key.key), range);
            entry["documentation"] = { { "kind", "markdown" }, { "value", markdown_key(key) } };
            items.push_back(std::move(entry));
        }
    }

    return items;
}

json_t toml_features_t::hover(const document_t& document, const json_t& params) {
    const source_file_t file(file_id_t{}, to_utf8(document.path), document.text);
    const std::string_view text = file.text();
    const std::uint32_t offset = m_positions.offset(file, params.at("position"));
    const auto [line_begin, line_end] = line_at(text, offset);
    const std::string_view line = text.substr(line_begin, line_end - line_begin);

    // A comment's words and a value's string's aren't keys or packages, though a quoted key is.
    if (const place_t at = place_at(text, offset);
        at.comment || (at.string_begin && (at.value || at.table_value))) {
        return nullptr;
    }

    // The word at the cursor, a key or a table's name.
    std::uint32_t begin = offset;
    std::uint32_t end = offset;
    while (begin > line_begin && is_key_char(text[begin - 1])) {
        --begin;
    }

    while (end < line_end && is_key_char(text[end])) {
        ++end;
    }

    if (begin == end) {
        return nullptr;
    }

    const std::string word(text.substr(begin, end - begin));
    const json_t range = m_positions.range(file, { .file = file.id(), .begin = begin, .end = end });
    const auto shown = [&](const std::string& markdown) -> json_t {
        return {
            { "contents", { { "kind", "markdown" }, { "value", markdown } } },
            { "range", range },
        };
    };
    // A table's name in its header, or a dependency's in its own table's, `[dependencies.physics]`.
    const std::optional<header_t> own_header = header_of(line);
    if (own_header) {
        for (const manifest_table_t& each : manifest_tables()) {
            if (each.name == own_header->table && each.name == word) {
                return shown(std::format("`[{}]`: {}.", each.name, each.description));
            }
        }

        if (own_header->table != "dependencies" || own_header->sub != word) {
            return nullptr;
        }
    }

    const std::optional<header_t> header = own_header ? own_header : table_at(text, offset);
    const std::string table = header ? header->table : std::string();
    const place_t place = place_at(text, begin);
    const bool own_table = !own_header && header && !header->sub.empty();

    // A key, of the table or of a dependency's, a native library's, or a registry's own table.
    if (!own_header && (!place.value || place.in_table)) {
        const bool inner = place.in_table || own_table;
        for (const manifest_key_t& key : manifest_keys()) {
            if (key.table == table && key.key == word &&
                (inner ||
                 (table != "dependencies" && table != "native" && table != "registries"))) {
                return shown(markdown_key(key));
            }
        }
    }

    // A dependency's name: the package, as its registry has it.
    const bool named = own_header || (!place.value && !own_table);
    if (table != "dependencies" || !named) {
        return nullptr;
    }

    const std::optional<std::string> index =
        registry_index(registry_named(text, line, own_header ? own_header->sub : ""),
                       registries_in(text), document.path.parent_path());
    if (!index) {
        return nullptr;
    }

    std::string description;
    bool asking = false;
    for (const found_package_t& package : search(*index, word, asking)) {
        if (package.name == word) {
            description = package.description;
        }
    }

    const std::vector<version_t> known = unyanked(versions(*index, word, asking));
    // While the registry's asked, the next hover has its answer.
    if (known.empty() && asking) {
        return nullptr;
    }

    if (known.empty()) {
        return shown(std::format("**{}**: {} has no such package, or can't be reached and the "
                                 "cache has nothing of it",
                                 word, *index));
    }

    // Offline, the description is the newest version's in the cache, if it's there.
    if (description.empty()) {
        const source_file_t cached(file_id_t{}, "project.toml", [&] {
            const std::ifstream in(package_cache_folder(*index, word, known.front().text()) /
                                       "project.toml",
                                   std::ios::binary);
            std::ostringstream bytes;
            bytes << in.rdbuf();
            return std::move(bytes).str();
        }());
        diagnostic_bag_t ignored;
        if (const std::optional<manifest_t> manifest = read_manifest(cached, ignored)) {
            description = manifest->description.value_or("");
        }
    }

    std::string markdown = std::format("**{}** {}", word, known.front().text());
    if (!description.empty()) {
        markdown += std::format("\n\n{}", description);
    }

    markdown += std::format("\n\nThe newest version on {}.", *index);
    return shown(markdown);
}

json_t toml_features_t::code_actions(const document_t& document, const json_t& params) {
    json_t out = json_t::array();
    const source_file_t file(file_id_t{}, to_utf8(document.path), document.text);
    const std::uint32_t begin = m_positions.offset(file, params.at("range").at("start"));
    const std::uint32_t end =
        std::max(begin, m_positions.offset(file, params.at("range").at("end")));
    const std::string uri = uri_of(document.path);

    // The fixes fb's errors have, from the last check.
    if (const std::shared_ptr<const compilation_t>& checked = document.checked) {
        const std::string wanted = file_key(document.path);
        for (const diagnostic_t& diagnostic : checked->diagnostics.diagnostics()) {
            const std::filesystem::path in = checked->disk_path(diagnostic.span.file);
            if (diagnostic.suggestions.empty() || in.empty() || file_key(in) != wanted ||
                diagnostic.span.begin > end || begin > diagnostic.span.end) {
                continue;
            }

            // Each suggestion is an action, the likeliest first and preferred.
            for (std::size_t i = 0; i < diagnostic.suggestions.size(); ++i) {
                const suggestion_t& suggestion = diagnostic.suggestions[i];
                json_t edits = json_t::array();
                for (const fix_t& fix : suggestion.fixes) {
                    const source_file_t& in_file = checked->sources.get(fix.span.file);
                    edits.push_back({
                        { "range", m_positions.range(in_file, fix.span) },
                        { "newText", fix.replacement },
                    });
                }

                out.push_back({
                    {
                        "title",
                        suggestion.title.empty() ? "Fix: " + diagnostic.message : suggestion.title,
                    },
                    { "kind", "quickfix" },
                    {
                        "diagnostics",
                        json_t::array({ diagnostic_json(*checked, diagnostic, m_positions) }),
                    },
                    { "isPreferred", i == 0 },
                    { "edit", { { "changes", { { uri, std::move(edits) } } } } },
                });
            }
        }
    }

    diagnostic_bag_t ignored;
    const std::optional<manifest_t> manifest = read_manifest(file, ignored);
    if (!manifest) {
        return out;
    }

    // A dependency whose range doesn't take its registry's newest version, as the registry last
    // said it, which a code action asked for as the cursor moves doesn't wait for.
    bool asking = false;
    const std::filesystem::path root = document.path.parent_path();
    for (const dependency_t& dependency : manifest->dependencies) {
        const span_t span = dependency.range_span;
        if (!dependency.range || dependency.name_span.begin > end ||
            begin > std::max(span.end, dependency.name_span.end)) {
            continue;
        }

        const std::optional<std::string> index =
            registry_index(dependency.registry, manifest->registries, root);
        const std::vector<version_t> known =
            index ? unyanked(versions(*index, dependency.name, asking)) : std::vector<version_t>{};
        if (known.empty() || dependency.range->matches(known.front())) {
            continue;
        }

        const std::string newest = known.front().text();
        out.push_back({
            { "title", std::format("Update {} to {}", dependency.name, newest) },
            { "kind", "quickfix" },
            {
                "edit",
                {
                    {
                        "changes",
                        {
                            {
                                uri,
                                json_t::array({
                                    {
                                        { "range", m_positions.range(file, span) },
                                        { "newText", std::format("\"{}\"", newest) },
                                    },
                                }),
                            },
                        },
                    },
                },
            },
        });
    }

    // What fb publish needs in [project], for a range in it: a library, as only libraries are
    // published, with a description and a license, which go at the end of the table.
    const auto [line_begin, line_end] = line_at(file.text(), begin);
    std::optional<header_t> header =
        header_of(file.text().substr(line_begin, line_end - line_begin));
    if (!header) {
        header = table_at(file.text(), begin);
    }

    if (!header || header->table != "project" || !header->sub.empty()) {
        return out;
    }

    std::error_code error;
    const bool executable =
        manifest->type == package_type_t::EXECUTABLE ||
        (!manifest->type &&
         (manifest->main_span ||
          std::filesystem::is_regular_file(root / path_from_utf8(manifest->main), error)));
    // A client that takes snippet edits gets placeholders to type over, and one that doesn't,
    // values that fb check and fb publish take, to change; each in the document's line breaks.
    const std::string_view newline = line_break(file.text());
    std::vector<std::string> missing;
    std::string lines;
    std::string snippet;
    int placeholder = 0;
    json_t edits = json_t::array();
    if (executable) {
        missing.emplace_back("type");
        if (manifest->type_span) {
            edits.push_back({
                { "range", m_positions.range(file, *manifest->type_span) },
                { "newText", "\"library\"" },
            });
        } else {
            lines += std::format("{}type = \"library\"", newline);
            snippet += std::format("{}type = \"library\"", newline);
        }
    }

    if (!manifest->description) {
        missing.emplace_back("description");
        lines += std::format("{}description = \"The {} package\"", newline, manifest->name);
        snippet += std::format("{}description = \"${{{}:What the package does}}\"", newline,
                               ++placeholder);
    }

    if (!manifest->license) {
        missing.emplace_back("license");
        lines += std::format("{}license = \"MIT\"", newline);
        snippet += std::format("{}license = \"${{{}:MIT}}\"", newline, ++placeholder);
    }

    if (missing.empty()) {
        return out;
    }

    const std::uint32_t at = keys_in(file.text(), "project").second;
    json_t edit;
    if (m_snippet_edits) {
        if (!snippet.empty()) {
            edits.push_back({
                { "range", m_positions.range(file, { .file = file.id(), .begin = at, .end = at }) },
                { "snippet", { { "kind", "snippet" }, { "value", snippet } } },
            });
        }

        edit = {
            {
                "documentChanges",
                json_t::array({
                    {
                        { "textDocument", { { "uri", uri }, { "version", document.version } } },
                        { "edits", std::move(edits) },
                    },
                }),
            },
        };
    } else {
        if (!lines.empty()) {
            edits.push_back(insertion(file, m_positions, at, lines));
        }

        edit = { { "changes", { { uri, std::move(edits) } } } };
    }

    std::string named;
    for (std::size_t i = 0; i < missing.size(); ++i) {
        if (i > 0) {
            named += i + 1 == missing.size() ? " and " : ", ";
        }

        named += missing[i];
    }

    out.push_back({
        { "title", std::format("Add what fb publish needs: {}", named) },
        { "kind", "quickfix" },
        { "edit", std::move(edit) },
    });
    return out;
}

} // namespace fb::lsp
