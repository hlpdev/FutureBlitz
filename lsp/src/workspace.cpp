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

void workspace_t::add_folders(const std::vector<std::filesystem::path>& folders) {
    std::unique_lock lock(m_mutex);
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

    lock.unlock();
    m_work.notify_all();
}

void workspace_t::remove_folders(const std::vector<std::filesystem::path>& folders) {
    const std::scoped_lock lock(m_mutex);
    for (const std::filesystem::path& folder : folders) {
        std::error_code error;
        const std::string key = file_key(
            std::filesystem::weakly_canonical(std::filesystem::absolute(folder, error), error));
        std::erase_if(m_folders,
                      [&](const std::filesystem::path& each) { return file_key(each) == key; });
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
    {
        const std::scoped_lock lock(m_mutex);
        const std::string key = file_key(path);
        m_documents[key] = { .version = version, .text = std::move(text) };
        ++m_generation;
        edited(key);
    }

    m_work.notify_all();
}

void workspace_t::edited(const std::string& key) {
    m_edited[key] = steady_t::now() + m_options.delay;
}

void workspace_t::close(const std::filesystem::path& path) {
    {
        const std::scoped_lock lock(m_mutex);
        const std::string key = file_key(path);
        m_documents.erase(key);
        m_latest.erase(key);
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
        ++m_generation;
        for (const std::filesystem::path& path : paths) {
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

    // A registry's package's build step never runs here, and its files are the ones a build of
    // one of the workspace's packages made.
    std::vector<std::filesystem::path> workspace;
    for (const unit_t& unit : m_units) {
        if (unit.kind == unit_t::kind_t::PACKAGE && owns(unit.root)) {
            workspace.push_back(unit.root);
        }
    }

    const std::uint64_t generation = m_generation;
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
        .examples = job.examples,
        .workspace = std::move(workspace),
    });
    std::shared_ptr<const compilation_t> compilation =
        result ? std::make_shared<const compilation_t>(std::move(*result)) : nullptr;
    const auto took =
        std::chrono::duration_cast<std::chrono::milliseconds>(steady_t::now() - start);
    lock.lock();
    --m_running;
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

    // The modules it had, by key, and what each imports.
    std::map<std::string, file_id_t> files;
    if (compilation) {
        const module_table_t& modules = compilation->ast.modules();
        for (const module_t& module : modules.modules()) {
            const std::filesystem::path path = compilation->disk_path(module.file);
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
                    const std::filesystem::path file = compilation->disk_path(modules.get(id).file);
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

    std::set<std::string> found;
    std::vector<std::string> work{ key };
    while (!work.empty()) {
        const std::string imported = work.back();
        work.pop_back();
        for (const auto& [file, imports] : m_imports) {
            if (imports.contains(imported) && file != key && candidates.contains(file) &&
                found.insert(file).second) {
                work.push_back(file);
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
