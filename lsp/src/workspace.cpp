#include "fb/lsp/workspace.hpp"

#include "fb/ast.hpp"
#include "fb/diagnostic.hpp"
#include "fb/driver.hpp"
#include "fb/lexer.hpp"
#include "fb/lsp/protocol.hpp"
#include "fb/manifest.hpp"
#include "fb/parser.hpp"
#include "fb/source.hpp"
#include "fb/toolchain.hpp"
#include "fb/utf8.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace fb::lsp {
namespace {

using steady_t = std::chrono::steady_clock;

/// The standard library's folder.
std::filesystem::path std_folder() {
    return toolchain().std;
}

/// Whether the file or folder `key` is the folder `folder` or in it, both as file_key() gives
/// them.
bool inside(const std::string_view key, const std::string_view folder) {
    return key == folder ||
           (key.starts_with(folder) && (folder.ends_with('/') || key[folder.size()] == '/'));
}

/// The text of a document parsed alone, as a compilation of its one file, whose diagnostics are
/// its syntax errors.
std::shared_ptr<compilation_t> parse_alone(const std::filesystem::path& path, std::string text) {
    auto parsed = std::make_shared<compilation_t>();
    parsed->file = parsed->sources.add(to_utf8(path), std::move(text));
    parsed->on_disk.resize(static_cast<std::size_t>(std::to_underlying(parsed->file)) + 1);
    parsed->on_disk.back() = path;
    const source_file_t& file = parsed->sources.get(parsed->file);
    std::vector<span_t> docs;
    const std::vector<token_t> tokens =
        lex(file, parsed->diagnostics, defined_symbols(build_mode_t::DEBUG), &docs);
    parse_into(file, tokens, parsed->diagnostics, parsed->ast, false, docs);
    return parsed;
}

/// Where the offsets of one text are in another, which an edit made of it: the same before
/// what changed, moved by as much as it grew after it, and nowhere in it.
class moved_t {
public:
    moved_t(const std::string_view from, const std::string_view to)
        : m_grown(static_cast<std::int64_t>(to.size()) - static_cast<std::int64_t>(from.size())) {
        const std::size_t shorter = std::min(from.size(), to.size());
        while (m_before < shorter && from[m_before] == to[m_before]) {
            ++m_before;
        }

        std::size_t after = 0;
        while (after < shorter - m_before &&
               from[from.size() - 1 - after] == to[to.size() - 1 - after]) {
            ++after;
        }

        m_after = from.size() - after;
    }

    /// Where `span` of the first text is in the second, nullopt when the edit was in it.
    [[nodiscard]] std::optional<span_t> operator()(span_t span) const {
        if (span.end <= m_before) {
            return span;
        }

        if (span.begin < m_after) {
            return std::nullopt;
        }

        span.begin = static_cast<std::uint32_t>(span.begin + m_grown);
        span.end = static_cast<std::uint32_t>(span.end + m_grown);
        return span;
    }

private:
    std::int64_t m_grown = 0;
    std::size_t m_before = 0;
    std::size_t m_after = 0;
};

/// The Public names each module of a check declares at its top level, by its path in the program,
/// each with the name an Import gives the module.
std::vector<std::pair<std::string, std::vector<export_t>>>
exports_of(const compilation_t& compilation) {
    const ast_t& ast = compilation.ast;
    const analysis_t& analysis = compilation.analysis;
    const std::set<stmt_id_t> top_level(ast.items().begin(), ast.items().end());
    std::map<file_id_t, std::vector<export_t>> by_file;
    for (const symbol_t& symbol : analysis.symbols) {
        if (symbol.span.empty() || symbol.owner != type_id_t::INVALID || !symbol.declaration ||
            !top_level.contains(*symbol.declaration) ||
            symbol.modifiers.visibility != visibility_t::PUBLIC) {
            continue;
        }

        std::string_view kind;
        switch (symbol.kind) {
        case symbol_kind_t::FUNCTION:
            kind = "Function";
            break;
        case symbol_kind_t::ALIAS:
            kind = "Alias";
            break;
        case symbol_kind_t::GLOBAL:
            kind = "Global";
            break;
        case symbol_kind_t::CONSTANT:
            kind = "Const";
            break;
        case symbol_kind_t::TYPE:
            switch (analysis.types.get(symbol.type).kind) {
            case type_kind_t::CLASS:
                kind = "Class";
                break;
            case type_kind_t::INTERFACE:
                kind = "Interface";
                break;
            case type_kind_t::ENUM:
                kind = "Enum";
                break;
            default:
                kind = "Struct";
                break;
            }

            break;
        default:
            continue;
        }

        std::vector<export_t>& names = by_file[symbol.span.file];
        if (std::ranges::none_of(names,
                                 [&](const export_t& name) { return name.name == symbol.name; })) {
            names.push_back({ .name = std::string(symbol.name), .kind = std::string(kind) });
        }
    }

    std::vector<std::pair<std::string, std::vector<export_t>>> found;
    for (const module_t& module : ast.modules().modules()) {
        if (module.name.empty() || module.name.contains('(')) {
            continue;
        }

        // A std module in a folder is the folder's, as "std.collections" for its map.fb.
        std::string imported = module.name;
        if (module.path.starts_with("std/") && std::ranges::count(module.path, '/') > 1) {
            imported = imported.substr(0, imported.rfind('.'));
        }

        std::vector<export_t> names = by_file[module.file];
        for (export_t& name : names) {
            name.module = imported;
        }

        found.emplace_back(module.path, std::move(names));
    }

    return found;
}

/// A file's text on disk, or nullopt when it can't be read.
std::optional<std::string> read_disk(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }

    return std::string{ std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}

/// Whether an entry of a folder is a link, which a walk doesn't follow.
bool is_link(const std::filesystem::directory_entry& entry) {
    std::error_code error;
    return entry.is_symlink(error);
}

/// `unit` with its files' keys.
unit_t with_keys(unit_t unit) {
    unit.keys.clear();
    for (const std::filesystem::path& file : unit.files) {
        unit.keys.insert(file_key(file));
    }

    return unit;
}

/// The .fb files from `folder` down, sorted: for the standard library, every one, and for a
/// package, but for its build/ folder, hidden folders, and the folders of other packages in it.
std::vector<std::filesystem::path> fb_files(const std::filesystem::path& folder,
                                            const bool package) {
    std::vector<std::filesystem::path> files;
    std::error_code error;
    for (auto entry = std::filesystem::recursive_directory_iterator(folder, error);
         !error && entry != std::filesystem::recursive_directory_iterator();
         entry.increment(error)) {
        std::error_code ignored;
        const std::string name = to_utf8(entry->path().filename());
        if (is_link(*entry)) {
            entry.disable_recursion_pending();
        } else if (entry->is_directory(ignored)) {
            if (name.starts_with('.') || (package && entry.depth() == 0 && name == "build") ||
                (package && std::filesystem::exists(entry->path() / "project.toml", ignored))) {
                entry.disable_recursion_pending();
            }
        } else if (entry->path().extension() == ".fb") {
            files.push_back(entry->path());
        }
    }

    std::ranges::sort(files);
    return files;
}

/// What unit a file at `path`, or a package's project.toml, is in: its kind and its root.
std::pair<unit_t::kind_t, std::filesystem::path> unit_place(const std::filesystem::path& path) {
    if (inside(file_key(path), file_key(std_folder()))) {
        return { unit_t::kind_t::STANDARD, std_folder() };
    }

    if (const std::optional<std::filesystem::path> root = find_package(path.parent_path())) {
        return { unit_t::kind_t::PACKAGE, *root };
    }

    return { unit_t::kind_t::LOOSE, path };
}

/// The unit of `kind` at `root`, with its files, and a package's project.toml read.
unit_t make_unit(const unit_t::kind_t kind, const std::filesystem::path& root) {
    if (kind == unit_t::kind_t::STANDARD) {
        return with_keys({ .kind = kind, .root = root, .files = fb_files(root, false) });
    }

    if (kind == unit_t::kind_t::LOOSE) {
        return with_keys({ .kind = kind, .root = root, .files = { root } });
    }

    unit_t unit{ .kind = kind, .root = root, .files = package_files(root) };
    source_manager_t sources;
    diagnostic_bag_t ignored;
    if (const std::expected<file_id_t, std::string> manifest =
            sources.load(root / "project.toml")) {
        if (const std::optional<manifest_t> read = read_manifest(sources.get(*manifest), ignored)) {
            unit.name = read->name;
            unit.width = read->format_width.value_or(default_format_width);
            const std::filesystem::path main = root / path_from_utf8(read->main);
            std::error_code error;
            if (std::filesystem::is_regular_file(main, error)) {
                unit.main = main;
            }
        }
    }

    return with_keys(std::move(unit));
}

} // namespace

bool has_top_level_code(const std::string& path, const std::string& text) {
    const source_file_t file(file_id_t{}, path, text);
    diagnostic_bag_t ignored;
    const std::vector<std::string_view> defined = defined_symbols(build_mode_t::DEBUG);
    const ast_t ast = parse(file, lex(file, ignored, defined), ignored);
    return std::ranges::any_of(ast.items(), [&](const stmt_id_t id) {
        const stmt_node_t& node = ast.get(id).node;
        const auto* variable = std::get_if<variable_stmt_t>(&node);
        return (variable != nullptr && variable->kind == variable_kind_t::LOCAL) ||
               (variable == nullptr && !std::holds_alternative<function_stmt_t>(node) &&
                !std::holds_alternative<type_stmt_t>(node) &&
                !std::holds_alternative<enum_stmt_t>(node) &&
                !std::holds_alternative<alias_stmt_t>(node) &&
                !std::holds_alternative<extend_stmt_t>(node) &&
                !std::holds_alternative<import_stmt_t>(node) &&
                !std::holds_alternative<extern_stmt_t>(node) &&
                !std::holds_alternative<error_stmt_t>(node));
    });
}

std::vector<std::filesystem::path> package_files(const std::filesystem::path& root) {
    return fb_files(root, true);
}

workspace_t::workspace_t(publish_t publish, const options_t options)
    : m_publish(std::move(publish)), m_options(options) {
    m_foreground = std::thread([this] { run_foreground(); });
    m_background = std::thread([this] { run_background(); });
}

workspace_t::~workspace_t() {
    {
        const std::scoped_lock lock(m_mutex);
        m_stopping = true;
    }

    m_work.notify_all();
    m_done.notify_all();
    m_foreground.join();
    m_background.join();
}

void workspace_t::watched(const bool watches) {
    const std::scoped_lock lock(m_mutex);
    m_watched = watches;
    rewatch();
}

void workspace_t::on_outside(watch_t watch) {
    const std::scoped_lock lock(m_mutex);
    m_watch = std::move(watch);
}

void workspace_t::watching(const std::vector<std::filesystem::path>& folders) {
    const std::scoped_lock lock(m_mutex);
    std::ranges::copy(folders, std::back_inserter(m_watched_outside));
    rewatch();
}

void workspace_t::rewatch() {
    std::vector<std::filesystem::path> folders = m_folders;
    folders.push_back(std_folder());
    std::ranges::copy(m_watched_outside, std::back_inserter(folders));
    m_disk.keep_only_under(folders);
    forget_file_keys();
}

void workspace_t::disk_changed() {
    m_disk.clear();
    forget_file_keys();
}

void workspace_t::add_folders(const std::vector<std::filesystem::path>& folders) {
    std::unique_lock lock(m_mutex);
    disk_changed();
    // Its folders are the cache's too, once it's told them.
    const auto told = [this] {
        if (m_watched) {
            rewatch();
        }
    };
    for (const std::filesystem::path& folder : folders) {
        std::error_code error;
        const std::filesystem::path absolute =
            std::filesystem::weakly_canonical(std::filesystem::absolute(folder, error), error);
        m_folders.push_back(absolute);
        // The package the folder is in, or the packages in it, and std when it's there.
        std::vector<std::filesystem::path> roots;
        if (const std::optional<std::filesystem::path> root = find_package(absolute)) {
            roots.push_back(*root);
        } else {
            for (auto entry = std::filesystem::recursive_directory_iterator(absolute, error);
                 !error && entry != std::filesystem::recursive_directory_iterator();
                 entry.increment(error)) {
                std::error_code ignored;
                const std::string name = to_utf8(entry->path().filename());
                // Hidden folders, and an editor extension's packages, have no packages of fb's.
                if (is_link(*entry) || (entry->is_directory(ignored) &&
                                        (name.starts_with('.') || name == "node_modules"))) {
                    entry.disable_recursion_pending();
                } else if (entry->is_directory(ignored) &&
                           std::filesystem::is_regular_file(entry->path() / "project.toml",
                                                            ignored)) {
                    roots.push_back(entry->path());
                }
            }
        }

        if (inside(file_key(std_folder()), file_key(absolute))) {
            roots.push_back(std_folder());
        }

        for (const std::filesystem::path& root : roots) {
            const std::size_t unit = unit_index(root / "project.toml");
            if (m_options.load_folders && !std::ranges::contains(m_loads, unit)) {
                m_loads.push_back(unit);
            }
        }
    }

    told();
    lock.unlock();
    m_work.notify_all();
}

void workspace_t::remove_folders(const std::vector<std::filesystem::path>& folders) {
    const std::scoped_lock lock(m_mutex);
    disk_changed();
    for (const std::filesystem::path& folder : folders) {
        std::error_code error;
        const std::string key = file_key(
            std::filesystem::weakly_canonical(std::filesystem::absolute(folder, error), error));
        std::erase_if(m_folders,
                      [&](const std::filesystem::path& each) { return file_key(each) == key; });
    }

    if (m_watched) {
        rewatch();
    }

    // The units that aren't the workspace's now aren't checked whole, and what's published for
    // their files goes, but for the open documents'.
    for (std::size_t i = 0; i < m_units.size(); ++i) {
        if (!owns(m_units[i].root)) {
            std::erase(m_loads, i);
            forget(m_units[i]);
        }
    }
}

void workspace_t::forget(const unit_t& unit) {
    if (std::ranges::any_of(unit.keys,
                            [this](const std::string& key) { return m_documents.contains(key); })) {
        return;
    }

    for (const std::string& key : unit.keys) {
        const auto published = m_published.find(key);
        const auto path = m_paths.find(key);
        if (published != m_published.end() && path != m_paths.end()) {
            published->second = ++m_generation;
            m_publish({ .path = path->second });
        }
    }
}

void workspace_t::open(const std::filesystem::path& path, const std::int64_t version,
                       std::string text) {
    {
        const std::scoped_lock lock(m_mutex);
        const std::string key = file_key(path);
        m_paths[key] = path;
        m_documents[key] = { .version = version, .text = std::move(text) };
        ++m_generation;
        const std::size_t units = m_units.size();
        const std::size_t unit = unit_index(path);
        // A package that isn't in the client's folders is checked whole too, for what imports
        // the file.
        if (unit == units && m_units[unit].kind == unit_t::kind_t::PACKAGE &&
            m_options.load_folders) {
            m_loads.push_back(unit);
        }

        m_edited[key] = steady_t::now();
    }

    m_work.notify_all();
}

void workspace_t::change(const std::filesystem::path& path, const std::int64_t version,
                         std::string text) {
    // Its syntax errors show as it's typed, before the check that waits for the next keystroke.
    const std::shared_ptr<compilation_t> parsed = parse_alone(path, text);
    {
        const std::scoped_lock lock(m_mutex);
        const std::string key = file_key(path);
        m_documents[key] = { .version = version, .text = std::move(text) };
        ++m_generation;
        show_syntax(key, path, version, parsed);
        edited(key);
    }

    m_work.notify_all();
}

void workspace_t::show_syntax(const std::string& key, const std::filesystem::path& path,
                              const std::int64_t version,
                              const std::shared_ptr<compilation_t>& parsed) {
    shown_t& shown = m_shown[key];
    const bool syntax = !parsed->diagnostics.diagnostics().empty();
    if (!syntax && !shown.syntax) {
        return;
    }

    // Its check's other diagnostics, where the edits since moved them, but for those an edit
    // was in, and their labels in its own text.
    std::vector<diagnostic_t> others;
    if (shown.compilation) {
        const moved_t moved(shown.text, parsed->source().text());
        for (const diagnostic_t* other : shown.others) {
            const std::optional<span_t> span = moved(other->span);
            if (!span) {
                continue;
            }

            diagnostic_t carried = *other;
            carried.span = { .file = parsed->file, .begin = span->begin, .end = span->end };
            carried.fixes.clear();
            std::erase_if(carried.labels, [&](label_t& label) {
                const std::optional<span_t> at =
                    label.span.file == other->span.file ? moved(label.span) : std::nullopt;
                if (at) {
                    label.span = { .file = parsed->file, .begin = at->begin, .end = at->end };
                }

                return !at;
            });
            others.push_back(std::move(carried));
        }
    }

    for (diagnostic_t& other : others) {
        parsed->diagnostics.report(std::move(other));
    }

    parsed->diagnostics.sort();
    published_t published{ .path = path, .version = version, .compilation = parsed };
    for (const diagnostic_t& diagnostic : parsed->diagnostics.diagnostics()) {
        published.diagnostics.push_back(&diagnostic);
    }

    // A check of an older text doesn't replace it, as it's newer.
    m_published[key] = m_generation;
    shown.syntax = syntax;
    m_publish(published);
}

void workspace_t::note_shown(const std::string& key, const std::string& text,
                             const std::shared_ptr<const compilation_t>& compilation,
                             const std::vector<const diagnostic_t*>& diagnostics) {
    // The check's syntax errors are those of a parse of its text alone, which a quick parse
    // shows itself.
    const std::shared_ptr<compilation_t> parsed = parse_alone(std::filesystem::path(), text);
    const std::vector<diagnostic_t>& syntax = parsed->diagnostics.diagnostics();
    shown_t& shown = m_shown[key];
    shown = { .compilation = compilation, .text = text, .syntax = !syntax.empty() };
    for (const diagnostic_t* diagnostic : diagnostics) {
        const bool parse = std::ranges::any_of(syntax, [&](const diagnostic_t& found) {
            return found.span.begin == diagnostic->span.begin &&
                   found.span.end == diagnostic->span.end && found.message == diagnostic->message;
        });
        if (!parse) {
            shown.others.push_back(diagnostic);
        }
    }
}

void workspace_t::edited(const std::string& key) {
    m_edited[key] = steady_t::now() + m_options.delay;
}

void workspace_t::close(const std::filesystem::path& path) {
    {
        const std::scoped_lock lock(m_mutex);
        // Its text on disk is read again, which it may have been saved over.
        disk_changed();
        const std::string key = file_key(path);
        m_documents.erase(key);
        m_latest.erase(key);
        m_shown.erase(key);
        m_edited.erase(key);
        ++m_generation;
        // Its text on disk may differ from what was checked, and a file outside the workspace has
        // nothing to say once it's closed.
        if (owns(path)) {
            m_alone.push_back(path);
            m_dependents[key] = steady_t::now() + m_options.dependents_delay;
        } else {
            m_published[key] = m_generation;
            m_publish({ .path = path });
            // Nor do its unit's other files, once none of them is open.
            std::erase(m_loads, unit_index(path));
            forget(m_units[unit_index(path)]);
        }
    }

    m_work.notify_all();
}

void workspace_t::changed_on_disk(const std::vector<std::filesystem::path>& paths) {
    {
        const std::scoped_lock lock(m_mutex);
        disk_changed();
        ++m_generation;
        // A folder that went takes its files, which the client may not name one by one.
        std::vector<std::filesystem::path> changed = paths;
        for (const std::filesystem::path& path : paths) {
            std::error_code error;
            if (path.extension() == ".fb" || path.filename() == "project.toml" ||
                std::filesystem::exists(path, error)) {
                continue;
            }

            const std::string folder = file_key(path);
            for (const auto& [key, known] : m_paths) {
                if (key != folder && inside(key, folder)) {
                    changed.push_back(known);
                }
            }
        }

        for (const std::filesystem::path& path : changed) {
            const std::string key = file_key(path);
            if (m_documents.contains(key)) {
                continue;
            }

            std::error_code error;
            const bool exists = std::filesystem::is_regular_file(path, error);
            if (path.filename() == "project.toml") {
                changed_manifest(path, exists);
                continue;
            }

            // A package's fb.lock, a library its project.toml names, or its prebuilt interface
            // file: what the package is made of, as its project.toml says.
            const std::string extension = to_utf8(path.extension());
            if (path.filename() == "fb.lock" || extension == ".dll" || extension == ".so" ||
                extension == ".dylib" || extension == ".a" || extension == ".lib" ||
                extension == ".fbi") {
                if (const std::optional<std::filesystem::path> root =
                        find_package(path.parent_path())) {
                    changed_manifest(*root / "project.toml", true);
                }

                continue;
            }

            // A .proto's module, which the files that import it are checked again for.
            if (extension == ".proto") {
                m_dependents[key] = steady_t::now() + m_options.dependents_delay;
                continue;
            }

            if (path.extension() != ".fb") {
                continue;
            }

            // unit_index() adds a file that's new to its package.
            m_paths[key] = path;
            unit_t& unit = m_units[unit_index(path)];
            if (!exists && unit.kind != unit_t::kind_t::LOOSE) {
                std::erase_if(unit.files, [&](const std::filesystem::path& file) {
                    return file_key(file) == key;
                });
                unit.keys.erase(key);
                m_unit_of.erase(key);
            }

            if (exists) {
                m_alone.push_back(path);
            } else {
                m_imports.erase(key);
                m_published[key] = m_generation;
                m_publish({ .path = path });
            }

            m_dependents[key] = steady_t::now() + m_options.dependents_delay;
        }
    }

    m_work.notify_all();
}

void workspace_t::changed_manifest(const std::filesystem::path& path, const bool exists) {
    // Every file under its folder is in the unit worked out again, which may be a new package,
    // or none, once a project.toml is made or deleted.
    const std::string root = file_key(path.parent_path());
    std::erase_if(m_unit_of, [&](const auto& entry) { return inside(entry.first, root); });
    for (std::size_t i = 0; i < m_units.size(); ++i) {
        unit_t& unit = m_units[i];
        if (unit.kind == unit_t::kind_t::PACKAGE && file_key(unit.root) == root) {
            unit = exists ? make_unit(unit_t::kind_t::PACKAGE, unit.root)
                          : unit_t{ .kind = unit_t::kind_t::PACKAGE, .root = unit.root };
            std::erase(m_loads, i);
        } else if (unit.kind == unit_t::kind_t::LOOSE && inside(file_key(unit.root), root)) {
            unit.files.clear();
            unit.keys.clear();
        }
    }

    if (exists) {
        const std::size_t unit = unit_index(path);
        if (owns(path) || std::ranges::any_of(m_units[unit].keys, [this](const std::string& key) {
                return m_documents.contains(key);
            })) {
            m_loads.push_back(unit);
        }
    }

    // The documents in it are checked again, in their units now.
    for (const auto& [key, document] : m_documents) {
        if (inside(key, root)) {
            m_latest.erase(key);
            m_edited[key] = steady_t::now();
        }
    }
}

std::optional<std::pair<std::string, std::int64_t>>
workspace_t::document(const std::filesystem::path& path) const {
    const std::scoped_lock lock(m_mutex);
    const auto found = m_documents.find(file_key(path));
    if (found == m_documents.end()) {
        return std::nullopt;
    }

    return std::pair{ found->second.text, found->second.version };
}

std::optional<std::string> workspace_t::text(const std::filesystem::path& path) const {
    if (std::optional<std::pair<std::string, std::int64_t>> open = document(path)) {
        return std::move(open->first);
    }

    return read_disk(path);
}

std::optional<checked_t> workspace_t::checked(const std::filesystem::path& path) {
    std::unique_lock lock(m_mutex);
    const std::string key = file_key(path);
    if (!m_paths.contains(key)) {
        m_paths[key] = path;
    }

    if (m_documents.contains(key)) {
        while (!m_stopping) {
            const auto document = m_documents.find(key);
            if (document == m_documents.end()) {
                break;
            }

            const auto latest = m_latest.find(key);
            if (latest != m_latest.end() && latest->second.generation > 0 &&
                latest->second.version == document->second.version) {
                return latest->second.checked;
            }

            // Its check is due now; a request doesn't wait for the next keystroke.
            if (const auto edited = m_edited.find(key); edited != m_edited.end()) {
                edited->second = steady_t::now();
                m_work.notify_all();
            } else if (m_running == 0) {
                m_edited[key] = steady_t::now();
                m_work.notify_all();
            }

            m_done.wait(lock);
        }

        return std::nullopt;
    }

    const std::shared_ptr<const compilation_t> compilation = run_job(lock, job_for(path));
    if (!compilation) {
        return std::nullopt;
    }

    for (const module_t& module : compilation->ast.modules().modules()) {
        if (file_key(compilation->disk_path(module.file)) == key) {
            return checked_t{ .compilation = compilation, .file = module.file };
        }
    }

    return std::nullopt;
}

std::vector<std::shared_ptr<const compilation_t>>
workspace_t::with_dependents(const std::filesystem::path& path) {
    std::unique_lock lock(m_mutex);
    const std::string key = file_key(path);
    if (!m_paths.contains(key)) {
        m_paths[key] = path;
    }

    std::set<std::string> keys = dependents(key);
    keys.insert(key);
    std::vector<std::shared_ptr<const compilation_t>> compilations;
    for (job_t& job : jobs_for(keys)) {
        job.examples = true;
        if (std::shared_ptr<const compilation_t> compilation = run_job(lock, job)) {
            compilations.push_back(std::move(compilation));
        }
    }

    return compilations;
}

std::vector<export_t> workspace_t::importable(const std::filesystem::path& path) {
    std::vector<export_t> found;
    {
        const std::scoped_lock lock(m_mutex);
        for (const auto& [module, names] : m_exports[unit_index(path)]) {
            std::ranges::copy(names, std::back_inserter(found));
        }
    }

    // std's modules that no check had, from the names they declare.
    const std::scoped_lock lock(m_std_names.mutex);
    for (const auto& [file, names] : m_std_names.files) {
        for (const std_name_t& name : names.names) {
            if (name.extended.empty() && !name.array_method) {
                found.push_back({ .name = name.name, .module = name.module, .kind = name.kind });
            }
        }
    }

    return found;
}

unit_t workspace_t::unit_of(const std::filesystem::path& path) {
    const std::scoped_lock lock(m_mutex);
    return m_units[unit_index(path)];
}

std::vector<unit_t> workspace_t::units() const {
    const std::scoped_lock lock(m_mutex);
    return m_units;
}

bool workspace_t::owns(const std::filesystem::path& path) const {
    const std::string key = file_key(path);
    return std::ranges::any_of(m_folders, [&](const std::filesystem::path& folder) {
        return inside(key, file_key(folder));
    });
}

void workspace_t::settle() {
    std::unique_lock lock(m_mutex);
    m_done.wait(lock, [this] {
        return m_stopping || (m_edited.empty() && m_dependents.empty() && m_alone.empty() &&
                              m_loads.empty() && m_running == 0);
    });
}

std::vector<check_record_t> workspace_t::history() const {
    const std::scoped_lock lock(m_mutex);
    return m_history;
}

void workspace_t::run_foreground() {
    std::unique_lock lock(m_mutex);
    while (!m_stopping) {
        if (m_edited.empty()) {
            m_work.wait(lock);
            continue;
        }

        const auto next =
            std::ranges::min_element(m_edited, {}, [](const auto& entry) { return entry.second; });
        if (next->second > steady_t::now()) {
            m_work.wait_until(lock, next->second);
            continue;
        }

        const std::string key = next->first;
        m_edited.erase(next);
        const std::filesystem::path path = m_paths.at(key);
        static_cast<void>(run_job(lock, job_for(path)));
        // Then what imports it, once the edits stop.
        m_dependents[key] = steady_t::now() + m_options.dependents_delay;
        m_work.notify_all();
        m_done.notify_all();
    }
}

void workspace_t::run_background() {
    std::unique_lock lock(m_mutex);
    while (!m_stopping) {
        const auto next = std::ranges::min_element(m_dependents, {},
                                                   [](const auto& entry) { return entry.second; });
        std::vector<job_t> jobs;
        if (next != m_dependents.end() && next->second <= steady_t::now()) {
            const std::string key = next->first;
            m_dependents.erase(next);
            // A document edited since is checked on its own anyway.
            std::set<std::string> keys = dependents(key);
            std::erase_if(keys,
                          [this](const std::string& each) { return m_edited.contains(each); });
            jobs = jobs_for(keys);
        } else if (!m_alone.empty()) {
            const std::filesystem::path path = m_alone.front();
            m_alone.erase(m_alone.begin());
            jobs.push_back(job_for(path));
        } else if (!m_loads.empty()) {
            const std::size_t unit = m_loads.front();
            m_loads.erase(m_loads.begin());
            const unit_t& whole = m_units[unit];
            jobs.push_back({
                .unit = unit,
                .roots = whole.files,
                .main = whole.kind == unit_t::kind_t::PACKAGE ? whole.main : std::nullopt,
            });
        } else {
            m_done.notify_all();
            if (next != m_dependents.end()) {
                m_work.wait_until(lock, next->second);
            } else {
                m_work.wait(lock);
            }

            continue;
        }

        for (const job_t& job : jobs) {
            if (m_stopping) {
                break;
            }

            if (!job.roots.empty()) {
                static_cast<void>(run_job(lock, job));
            }
        }

        m_done.notify_all();
    }
}

std::shared_ptr<const compilation_t> workspace_t::run_job(std::unique_lock<std::mutex>& lock,
                                                          const job_t& job) {
    // What the check reads: the documents' texts as they are now.
    std::map<std::string, std::string> texts;
    std::map<std::string, std::int64_t> versions;
    for (const auto& [key, document] : m_documents) {
        texts.emplace(key, document.text);
        versions.emplace(key, document.version);
    }

    const std::uint64_t generation = m_generation;
    disk_cache_t* disk = m_watched ? &m_disk : nullptr;
    ++m_running;
    lock.unlock();
    const auto start = steady_t::now();
    std::expected<compilation_t, std::string> result = check_files({
        .files = job.roots,
        .main = job.main,
        .read = [&texts](const std::filesystem::path& file) -> std::optional<std::string> {
            const auto found = texts.find(file_key(file));
            return found != texts.end() ? std::optional(found->second) : std::nullopt;
        },
        .cache = &m_std_names,
        .disk = disk,
        .examples = job.examples,
    });
    std::shared_ptr<const compilation_t> compilation =
        result ? std::make_shared<const compilation_t>(std::move(*result)) : nullptr;
    const auto took =
        std::chrono::duration_cast<std::chrono::milliseconds>(steady_t::now() - start);
    // What its modules declare that others can import, for completion, found unlocked.
    std::vector<std::pair<std::string, std::vector<export_t>>> exported;
    if (compilation) {
        exported = exports_of(*compilation);
    }

    lock.lock();
    --m_running;
    for (auto& [path, names] : exported) {
        m_exports[job.unit][path] = std::move(names);
    }

    // The folders of packages it read outside the client's, which only a watcher of their own
    // sees.
    if (compilation && m_watched && m_watch) {
        std::vector<std::filesystem::path> outside;
        for (const module_t& module : compilation->ast.modules().modules()) {
            const std::filesystem::path path = compilation->disk_path(module.file);
            if (path.empty() || module.path.starts_with("std/") || owns(path)) {
                continue;
            }

            const std::filesystem::path root =
                find_package(path.parent_path()).value_or(path.parent_path());
            if (m_outside.insert(file_key(root)).second) {
                outside.push_back(root);
            }
        }

        if (!outside.empty()) {
            m_watch(outside);
        }
    }

    m_history.push_back({
        .roots = job.roots,
        .modules = compilation ? compilation->ast.modules().size() : 0,
        .took = took,
    });
    // A long session's, kept to the latest.
    constexpr std::size_t kept = 10'000;
    if (m_history.size() > kept) {
        m_history.erase(m_history.begin());
    }

    // The modules it had, by key, and what each imports. A .proto's module is the .proto's.
    std::map<std::string, file_id_t> files;
    if (compilation) {
        const auto where = [&](const file_id_t file) {
            const std::filesystem::path path = compilation->disk_path(file);
            return path.empty() ? compilation->made_from_path(file) : path;
        };
        const module_table_t& modules = compilation->ast.modules();
        for (const module_t& module : modules.modules()) {
            const std::filesystem::path path = where(module.file);
            if (path.empty()) {
                continue;
            }

            const std::string key = file_key(path);
            files.emplace(key, module.file);
            m_paths.try_emplace(key, path);
            // Not over a newer check's, nor from a document's text before its edit.
            const auto document = m_documents.find(key);
            const auto read = versions.find(key);
            const bool current =
                document == m_documents.end()
                    ? read == versions.end()
                    : read != versions.end() && read->second == document->second.version;
            std::uint64_t& recorded = m_import_generations[key];
            if (!current || recorded > generation) {
                continue;
            }

            recorded = generation;
            std::set<std::string>& imports = m_imports[key];
            imports.clear();
            for (const module_import_t& imported : module.imports) {
                for (const module_id_t id : imported.modules) {
                    const std::filesystem::path file = where(modules.get(id).file);
                    if (!file.empty()) {
                        imports.insert(file_key(file));
                    }
                }
            }
        }
    }

    // What's published, for one of the roots or a project.toml: its diagnostics, unless a newer
    // check has published its own.
    const auto publish = [&](const std::string& key, const std::filesystem::path& path,
                             const std::optional<file_id_t> file) {
        const auto document = m_documents.find(key);
        const auto read = versions.find(key);
        const bool current =
            document == m_documents.end()
                ? read == versions.end()
                : read != versions.end() && read->second == document->second.version;
        if (!current) {
            return;
        }

        if (document != m_documents.end()) {
            latest_t& latest = m_latest[key];
            if (generation >= latest.generation) {
                latest = {
                    .checked = file && compilation ? std::optional<checked_t>(checked_t{
                                                         .compilation = compilation,
                                                         .file = *file,
                                                     })
                                                   : std::nullopt,
                    .version = document->second.version,
                    .generation = std::max<std::uint64_t>(generation, 1),
                };
            }
        }

        const auto published = m_published.find(key);
        if (published != m_published.end() && published->second > generation) {
            return;
        }

        m_published[key] = generation;
        published_t diagnostics{
            .path = path,
            .version = document != m_documents.end() ? std::optional(document->second.version)
                                                     : std::nullopt,
            .compilation = compilation,
        };
        if (compilation && file) {
            for (const diagnostic_t& diagnostic : compilation->diagnostics.diagnostics()) {
                if (diagnostic.span.file == *file) {
                    diagnostics.diagnostics.push_back(&diagnostic);
                }
            }
        }

        if (document != m_documents.end()) {
            note_shown(key, texts.at(key), compilation, diagnostics.diagnostics);
        }

        m_publish(diagnostics);
    };

    for (const std::filesystem::path& root : job.roots) {
        const std::string key = file_key(root);
        const auto found = files.find(key);
        if (compilation && found != files.end()) {
            const std::optional<module_id_t> module = compilation->ast.modules().of(found->second);
            // Another main file's top-level code was left out, so it's checked on its own.
            if (module && compilation->ast.modules().get(*module).other_main) {
                if (!std::ranges::contains(m_alone, root)) {
                    m_alone.push_back(root);
                }

                continue;
            }
        }

        publish(key, root, found != files.end() ? std::optional(found->second) : std::nullopt);
    }

    // A project.toml's mistakes, which every check of its package reads.
    if (compilation) {
        for (std::size_t i = 0; i < compilation->on_disk.size(); ++i) {
            const std::filesystem::path& path = compilation->on_disk[i];
            if (path.filename() == "project.toml") {
                publish(file_key(path), path, static_cast<file_id_t>(i));
            }
        }
    }

    m_done.notify_all();
    return compilation;
}

std::size_t workspace_t::unit_index(const std::filesystem::path& path) {
    const std::string key = file_key(path);
    if (const auto known = m_unit_of.find(key); known != m_unit_of.end()) {
        return known->second;
    }

    const auto [kind, root] = unit_place(path);
    const std::string root_key = file_key(root);
    std::size_t index = 0;
    while (index < m_units.size() &&
           (m_units[index].kind != kind || file_key(m_units[index].root) != root_key)) {
        ++index;
    }

    if (index == m_units.size()) {
        m_units.push_back(make_unit(kind, root));
    }

    // A file a package's folder has now, which wasn't there when its files were found.
    unit_t& found = m_units[index];
    if (found.kind != unit_t::kind_t::LOOSE && path.extension() == ".fb" &&
        found.keys.insert(key).second) {
        found.files.push_back(path);
        std::ranges::sort(found.files);
    }

    m_unit_of.emplace(key, index);
    return index;
}

workspace_t::job_t workspace_t::job_for(const std::filesystem::path& path) {
    const std::size_t unit = unit_index(path);
    const auto document = m_documents.find(file_key(path));
    const std::optional<std::string> text =
        document != m_documents.end() ? std::optional(document->second.text) : read_disk(path);
    const bool code = m_units[unit].kind == unit_t::kind_t::PACKAGE && text &&
                      has_top_level_code(to_utf8(path), *text);
    return { .unit = unit, .roots = { path }, .main = code ? std::optional(path) : std::nullopt };
}

std::set<std::string> workspace_t::dependents(const std::string& key) const {
    // What a check can have: the files of the units in the client's folders, and the documents.
    std::set<std::string> candidates;
    for (const unit_t& unit : m_units) {
        if (owns(unit.root)) {
            candidates.insert(unit.keys.begin(), unit.keys.end());
        }
    }

    for (const auto& [document, ignored] : m_documents) {
        candidates.insert(document);
    }

    // Every program loads std/core/ and std.collections.map, whether or not it imports them.
    const std::string standard = file_key(std_folder());
    if (inside(key, standard + "/core") || key == standard + "/collections/map.fb") {
        candidates.erase(key);
        return candidates;
    }

    // Through files no check has as a root, as a .proto another imports, or a package's outside
    // the client's folders.
    std::set<std::string> found;
    std::set<std::string> walked{ key };
    std::vector<std::string> work{ key };
    while (!work.empty()) {
        const std::string imported = work.back();
        work.pop_back();
        for (const auto& [file, imports] : m_imports) {
            if (imports.contains(imported) && walked.insert(file).second) {
                work.push_back(file);
                if (candidates.contains(file)) {
                    found.insert(file);
                }
            }
        }
    }

    return found;
}

std::vector<workspace_t::job_t> workspace_t::jobs_for(const std::set<std::string>& keys) {
    std::map<std::size_t, std::vector<std::filesystem::path>> by_unit;
    for (const std::string& key : keys) {
        const auto path = m_paths.find(key);
        if (path != m_paths.end()) {
            by_unit[unit_index(path->second)].push_back(path->second);
        }
    }

    std::vector<job_t> jobs;
    for (auto& [unit, roots] : by_unit) {
        if (m_units[unit].kind == unit_t::kind_t::LOOSE) {
            for (const std::filesystem::path& root : roots) {
                jobs.push_back(job_for(root));
            }

            continue;
        }

        if (roots.size() == 1) {
            jobs.push_back(job_for(roots.front()));
            continue;
        }

        const std::optional<std::filesystem::path>& main = m_units[unit].main;
        const bool has_main =
            main && std::ranges::any_of(roots, [&](const std::filesystem::path& root) {
                return file_key(root) == file_key(*main);
            });
        jobs.push_back({
            .unit = unit,
            .roots = std::move(roots),
            .main = has_main && m_units[unit].kind == unit_t::kind_t::PACKAGE ? main : std::nullopt,
        });
    }

    return jobs;
}

} // namespace fb::lsp
