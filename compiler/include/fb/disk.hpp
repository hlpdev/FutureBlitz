#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fb {

/// One entry of a folder, and what's at it, following links.
struct disk_entry_t {
    std::filesystem::path path;
    bool file = false;
    bool folder = false;
};

/// What the front end asks the disk, kept while the disk doesn't change: where a path really
/// leads, what's at it, a folder's entries, and a source file's text. A front end asks it once
/// for each, though it asks of a path again for each Import. The LSP keeps one for its checks,
/// which it empties when the client says files changed; each of fb's runs of the front end has
/// one of its own (disk_scope_t). Threads can share it.
struct disk_cache_t {
    struct status_t {
        std::filesystem::file_type type = std::filesystem::file_type::none;
        std::error_code error;
    };

    /// A path as the system spells it, which the answers are kept by: comparing these is quicker
    /// than comparing paths, part by part.
    using key_t = std::filesystem::path::string_type;

    std::mutex mutex;
    std::unordered_map<key_t, std::pair<std::filesystem::path, std::error_code>> canonical;
    std::unordered_map<key_t, std::optional<std::filesystem::path>> real;
    std::unordered_map<key_t, status_t> statuses;
    std::unordered_map<key_t, std::pair<bool, std::error_code>> equivalent;
    std::unordered_map<key_t,
                       std::pair<std::shared_ptr<const std::vector<disk_entry_t>>, std::error_code>>
        entries;
    std::unordered_map<key_t, std::vector<std::filesystem::path>> files_under;
    std::unordered_map<key_t, std::shared_ptr<const std::optional<std::string>>> texts;
    /// How many times it was emptied, so an answer asked for before it last was isn't kept.
    std::uint64_t clears = 0;
    /// The folders it keeps answers about, as keeps() compares them, when it keeps only some.
    std::optional<std::vector<std::string>> watched;

    /// Forgets everything, as the disk may have changed.
    void clear();

    /// Keeps answers only about paths in `folders` from now, as the LSP's cache does about the
    /// folders whose changes the client says, and forgets the rest.
    void keep_only_under(const std::vector<std::filesystem::path>& folders);

    /// Whether it keeps answers about `path`: one in a folder it was told of, or any without.
    [[nodiscard]] bool keeps(const std::filesystem::path& path);

    /// What `ask` gives for `key` among `answers`: the answer kept, or `ask`'s, which is kept
    /// unless the cache was emptied while it asked, as the disk may have changed since. The
    /// disk:: functions answer through it.
    template <typename Value, typename Ask>
    Value remember(std::unordered_map<key_t, Value> disk_cache_t::* answers, const key_t& key,
                   Ask ask) {
        return remember(answers, key, std::move(ask), [](const Value&) { return true; });
    }

    /// remember(), keeping only an answer `keep` says it can.
    template <typename Value, typename Ask, typename Keep>
    Value remember(std::unordered_map<key_t, Value> disk_cache_t::* answers, const key_t& key,
                   Ask ask, Keep keep) {
        std::uint64_t asked_at = 0;
        {
            const std::scoped_lock lock(mutex);
            const std::unordered_map<key_t, Value>& known = this->*answers;
            if (const auto found = known.find(key); found != known.end()) {
                return found->second;
            }

            asked_at = clears;
        }

        // Asked without the lock, so one slow answer doesn't hold up another thread's, and
        // `keep` too, which may ask keeps().
        Value value = ask();
        if (!keep(value)) {
            return value;
        }

        const std::scoped_lock lock(mutex);
        if (clears != asked_at) {
            return value;
        }

        return (this->*answers).try_emplace(key, std::move(value)).first->second;
    }
};

/// Puts a disk cache in use on this thread while it lives: `cache`, or, when it's null, the one
/// in use already, or else one of its own, so a run of the front end asks the disk once.
class disk_scope_t {
public:
    explicit disk_scope_t(disk_cache_t* cache = nullptr);
    disk_scope_t(const disk_scope_t&) = delete;
    disk_scope_t& operator=(const disk_scope_t&) = delete;
    disk_scope_t(disk_scope_t&&) = delete;
    disk_scope_t& operator=(disk_scope_t&&) = delete;
    ~disk_scope_t();

private:
    std::unique_ptr<disk_cache_t> m_own;
    disk_cache_t* m_previous = nullptr;
};

/// The disk's answers, through the cache in use on this thread, or straight from the disk without
/// one. Each is std::filesystem's of the same name, or fb's own.
namespace disk {

[[nodiscard]] std::filesystem::path weakly_canonical(const std::filesystem::path& path,
                                                     std::error_code& error);

/// real_path()'s.
[[nodiscard]] std::optional<std::filesystem::path> real_path(const std::filesystem::path& path);

[[nodiscard]] std::filesystem::file_type type(const std::filesystem::path& path,
                                              std::error_code& error);
[[nodiscard]] bool exists(const std::filesystem::path& path, std::error_code& error);
[[nodiscard]] bool is_regular_file(const std::filesystem::path& path, std::error_code& error);
[[nodiscard]] bool is_directory(const std::filesystem::path& path, std::error_code& error);
[[nodiscard]] bool equivalent(const std::filesystem::path& first,
                              const std::filesystem::path& second, std::error_code& error);

/// A folder's entries, as std::filesystem::directory_iterator lists them, with the error that
/// stopped it, if one did. The cache's list is shared, as a front end asks for a folder's for
/// each Import.
[[nodiscard]] std::shared_ptr<const std::vector<disk_entry_t>>
entries(const std::filesystem::path& folder, std::error_code& error);

/// The files under a folder, its folders' too, as std::filesystem::recursive_directory_iterator
/// lists them, without following links to folders.
[[nodiscard]] std::vector<std::filesystem::path> files_under(const std::filesystem::path& folder);

/// A file's text, or nullopt when it isn't a file or can't be read.
[[nodiscard]] std::optional<std::string> read(const std::filesystem::path& path);

} // namespace disk

} // namespace fb
