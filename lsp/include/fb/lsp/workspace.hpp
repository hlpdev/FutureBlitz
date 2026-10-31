#pragma once

#include "fb/diagnostic.hpp"
#include "fb/disk.hpp"
#include "fb/driver.hpp"
#include "fb/format.hpp"
#include "fb/lsp/features.hpp"
#include "fb/source.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace fb::lsp {

/// A file in a check's compilation.
struct checked_t {
    std::shared_ptr<const compilation_t> compilation;
    file_id_t file{};
};

/// What one compilation covers: a package, with its tests/ folder, the standard library, or a
/// .fb file outside a package, with what it imports. Making its set can allocate, which
/// clang-tidy reports as an exception escaping.
struct unit_t { // NOLINT(bugprone-exception-escape)
    enum class kind_t : std::uint8_t {
        PACKAGE,
        STANDARD,
        LOOSE,
    };

    kind_t kind = kind_t::LOOSE;
    /// The package's folder, std's, or the loose file.
    std::filesystem::path root;
    /// The package's name, which its project.toml gives.
    std::string name;
    /// Its .fb files, sorted: a package's from its folder down, but for its build/ folder, hidden
    /// folders, and other packages' folders in it.
    std::vector<std::filesystem::path> files;
    /// Their keys, as file_key() gives them.
    std::set<std::string> keys;
    /// The package's main file, which its project.toml names, when it's there.
    std::optional<std::filesystem::path> main;
    /// The line width its code is formatted to, from its project.toml's `[format] width`.
    std::uint32_t width = default_format_width;
};

/// What the server publishes for a file: the diagnostics of the check it was last part of,
/// which are in `compilation`, whose files their spans are in.
struct published_t {
    std::filesystem::path path;
    /// The open document's version it was checked at, or nullopt for its text on disk.
    std::optional<std::int64_t> version;
    std::shared_ptr<const compilation_t> compilation;
    std::vector<const diagnostic_t*> diagnostics;
};

/// One check the workspace ran, for its tests.
struct check_record_t {
    /// The files it checked, as modules of their own, and how many modules it had with what they
    /// import.
    std::vector<std::filesystem::path> roots;
    std::size_t modules = 0;
    std::chrono::milliseconds took{};
};

/// The LSP's front end, which re-checks only what an edit touches. A check is the driver's
/// check_files() of some files of one unit, the roots, with what they import: an edit checks the
/// edited file, soon after it, and then, in the background, the files that import it, directly
/// or not, as the import graph of earlier checks says. The units of the client's folders are
/// each checked whole in the background once, which gives that graph and every file's
/// diagnostics. docs/tooling.md's "The LSP (step 4)" has the design.
///
/// It's safe to use from several threads; its own two run the checks: one for edited documents,
/// and one for the rest.
class workspace_t {
public:
    struct options_t {
        /// How long an edited document's check waits for the next edit.
        std::chrono::milliseconds delay{ 150 };
        /// How long the check of what imports an edited file waits.
        std::chrono::milliseconds dependents_delay{ 400 };
        /// Whether the units of the client's folders are checked whole as they're added.
        bool load_folders = true;
    };

    using publish_t = std::function<void(const published_t&)>;

    explicit workspace_t(publish_t publish, options_t options);
    workspace_t(const workspace_t&) = delete;
    workspace_t& operator=(const workspace_t&) = delete;
    workspace_t(workspace_t&&) = delete;
    workspace_t& operator=(workspace_t&&) = delete;
    /// Stops the checks, waiting for the ones running.
    ~workspace_t();

    /// Whether the client says when files change, so the checks can share what they know of
    /// the disk, which the workspace forgets then; without it, each check asks the disk itself.
    void watched(bool watches);

    /// What asks the client to watch folders outside its own, which checks found packages in.
    using watch_t = std::function<void(const std::vector<std::filesystem::path>&)>;
    void on_outside(watch_t watch);

    /// Folders besides the client's own whose files it says change, so what the checks know of
    /// them can be shared too.
    void watching(const std::vector<std::filesystem::path>& folders);

    /// The client's folders: each package in them, and std when it's among them.
    void add_folders(const std::vector<std::filesystem::path>& folders);
    /// Folders the client closed, whose files aren't the workspace's any more.
    void remove_folders(const std::vector<std::filesystem::path>& folders);

    void open(const std::filesystem::path& path, std::int64_t version, std::string text);
    void change(const std::filesystem::path& path, std::int64_t version, std::string text);
    /// Its text on disk counts again.
    void close(const std::filesystem::path& path);
    /// Files that changed on disk, were made, or were deleted, and aren't open: each is checked
    /// again with what imports it, and a project.toml's package whole.
    void changed_on_disk(const std::vector<std::filesystem::path>& paths);

    /// An open document's text and version. nullopt when it isn't open.
    [[nodiscard]] std::optional<std::pair<std::string, std::int64_t>>
    document(const std::filesystem::path& path) const;

    /// The text the front end reads for a file: its document's, or its text on disk.
    [[nodiscard]] std::optional<std::string> text(const std::filesystem::path& path) const;

    /// The latest check of a file. For an open document, it's the one of its current text,
    /// which this waits for; another file that no check has yet is checked now. nullopt when it
    /// can't be checked.
    [[nodiscard]] std::optional<checked_t> checked(const std::filesystem::path& path);

    /// Checks that together hold the file at `path` and every file of the workspace that imports
    /// it, directly or not, with their current texts: one for each unit they're in, for
    /// references and renames, with the examples of a package's files' doc comments, whose
    /// code uses its names too.
    [[nodiscard]] std::vector<std::shared_ptr<const compilation_t>>
    with_dependents(const std::filesystem::path& path);

    /// The unit a file is in, which a check of it would cover.
    [[nodiscard]] unit_t unit_of(const std::filesystem::path& path);

    /// The Public names of the modules the file at `path` can import, as the checks of its unit
    /// found them, and std's.
    [[nodiscard]] std::vector<export_t> importable(const std::filesystem::path& path);

    /// Every unit the workspace has: its folders' and those of the documents opened.
    [[nodiscard]] std::vector<unit_t> units() const;

    /// Whether a file is in one of the client's folders, so it's the workspace's own, which a
    /// rename can change.
    [[nodiscard]] bool owns(const std::filesystem::path& path) const;

    /// Waits until no check waits or runs, as the tests do before they look.
    void settle();

    [[nodiscard]] std::vector<check_record_t> history() const;

private:
    struct document_t {
        std::int64_t version = 0;
        std::string text;
    };

    /// A check of `roots`, files of the unit `unit` in `m_units`.
    struct job_t {
        std::size_t unit = 0;
        std::vector<std::filesystem::path> roots;
        std::optional<std::filesystem::path> main;
        /// With the files' examples, as check_request_t has them.
        bool examples = false;
    };

    /// What an open document's last check has: it, and the version it checked.
    struct latest_t {
        std::optional<checked_t> checked;
        std::int64_t version = 0;
        std::uint64_t generation = 0;
    };

    publish_t m_publish;
    options_t m_options;
    mutable std::mutex m_mutex;
    /// Wakes the workers when there's work, and the waiters when a check is done.
    std::condition_variable m_work;
    std::condition_variable m_done;
    std::vector<std::filesystem::path> m_folders;
    std::vector<unit_t> m_units;
    std::map<std::string, document_t> m_documents;
    /// The documents edited since their last check, by key, with when their check is due.
    std::map<std::string, std::chrono::steady_clock::time_point> m_edited;
    /// Files whose dependents' check is due, by key, with when, and the units to check whole.
    std::map<std::string, std::chrono::steady_clock::time_point> m_dependents;
    std::vector<std::size_t> m_loads;
    /// Files to check on their own in the background, as a main file each: those a check had as
    /// another main file, whose top-level code it left out, and files changed on disk.
    std::vector<std::filesystem::path> m_alone;
    std::map<std::string, latest_t> m_latest;
    /// What each file imports, by key, from the latest check that had it, and that check's
    /// generation.
    std::map<std::string, std::set<std::string>> m_imports;
    /// What the embeds of each file's latest check as a root named, files and folders, by keys,
    /// whose changes check it again.
    std::map<std::string, std::set<std::string>> m_embeds;
    std::map<std::string, std::uint64_t> m_import_generations;
    /// Each file's paths by key, as the checks have found them.
    std::map<std::string, std::filesystem::path> m_paths;
    /// The unit each file is in, by key, once unit_index() has found it.
    std::map<std::string, std::size_t> m_unit_of;
    /// The generation of the check each file's published diagnostics are from.
    std::map<std::string, std::uint64_t> m_published;
    /// Counts the edits, so a check knows which texts it read.
    std::uint64_t m_generation = 0;
    std::size_t m_running = 0;
    bool m_stopping = false;
    std::vector<check_record_t> m_history;
    /// What std's modules declare, which every check needs, and only the first works out.
    std_names_cache_t m_std_names;
    /// What a check published for an open document, with the text it read, so a parse of the
    /// document alone, as it's typed, can show its syntax errors with the check's other
    /// diagnostics, where the edits since have moved them.
    struct shown_t {
        std::shared_ptr<const compilation_t> compilation;
        std::string text;
        /// The check's diagnostics of the document, but for those a parse of its text gives.
        std::vector<const diagnostic_t*> others;
        /// Whether what's published for the document now has syntax errors.
        bool syntax = false;
    };

    /// By the open documents' keys.
    std::map<std::string, shown_t> m_shown;
    /// The Public names each module declares, by its path in the program, by the unit whose
    /// checks found them.
    std::map<std::size_t, std::map<std::string, std::vector<export_t>>> m_exports;
    /// What the checks know of the disk, when the client says when files change.
    disk_cache_t m_disk;
    bool m_watched = false;
    watch_t m_watch;
    /// The keys of the folders outside the client's whose watchers were asked for, and those it
    /// said it watches.
    std::set<std::string> m_outside;
    std::vector<std::filesystem::path> m_watched_outside;
    std::thread m_foreground;
    std::thread m_background;

    void run_foreground();
    void run_background();
    /// Forgets what the checks know of the disk, and the files' keys, as it may have changed.
    void disk_changed();
    /// Tells the cache the folders it keeps answers about: those the client watches, and std's.
    void rewatch();
    /// Publishes the syntax errors of `parsed`, a parse of the document `key` alone at `version`,
    /// with its last check's other diagnostics, when it has some, or what's published has.
    void show_syntax(const std::string& key, const std::filesystem::path& path,
                     std::int64_t version, const std::shared_ptr<compilation_t>& parsed);
    /// Notes what a check published for the open document `key`, which read `text`.
    void note_shown(const std::string& key, const std::string& text,
                    const std::shared_ptr<const compilation_t>& compilation,
                    const std::vector<const diagnostic_t*>& diagnostics);
    /// Runs `job` with the lock unlocked, and takes in what it found. `foreground` checks are of
    /// edited documents.
    std::shared_ptr<const compilation_t> run_job(std::unique_lock<std::mutex>& lock,
                                                 const job_t& job);
    /// The index in `m_units` of the unit `path` is in, adding one when none has it.
    std::size_t unit_index(const std::filesystem::path& path);
    /// The job that checks the file at `path` on its own.
    job_t job_for(const std::filesystem::path& path);
    /// The files of the workspace's units that import the file `key`, directly or not.
    [[nodiscard]] std::set<std::string> dependents(const std::string& key) const;
    /// Jobs that check `keys`, one for each unit they're in.
    std::vector<job_t> jobs_for(const std::set<std::string>& keys);
    void edited(const std::string& key);
    /// A project.toml at `path` that's been changed, made, or deleted, as `exists` says.
    void changed_manifest(const std::filesystem::path& path, bool exists);
    /// Takes back what's published for a unit's files, when none of them is open.
    void forget(const unit_t& unit);
};

/// Whether a file's top-level statements include code, besides declarations, which only a main
/// file runs.
[[nodiscard]] bool has_top_level_code(const std::string& path, const std::string& text);

/// A package's .fb files, as unit_t::files has them.
[[nodiscard]] std::vector<std::filesystem::path> package_files(const std::filesystem::path& root);

} // namespace fb::lsp
