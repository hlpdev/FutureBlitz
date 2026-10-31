#include "fb/disk.hpp"

#include "fb/process.hpp"

#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fb {
namespace {

/// The cache in use on this thread, if any.
thread_local disk_cache_t* current = nullptr;

/// What `ask` gives for `key`, from the cache in use, which asks once.
template <typename Value, typename Ask>
Value remembered(std::unordered_map<disk_cache_t::key_t, Value> disk_cache_t::* answers,
                 const disk_cache_t::key_t& key, Ask ask) {
    if (current == nullptr) {
        return ask();
    }

    return current->remember(answers, key, std::move(ask));
}

/// Two paths as one key, apart by a character no path has.
disk_cache_t::key_t pair_key(const std::filesystem::path& first,
                             const std::filesystem::path& second) {
    disk_cache_t::key_t key = first.native();
    key += disk_cache_t::key_t::value_type{};
    key += second.native();
    return key;
}

} // namespace

void disk_cache_t::clear() {
    const std::scoped_lock lock(mutex);
    ++clears;
    canonical.clear();
    real.clear();
    statuses.clear();
    equivalent.clear();
    entries.clear();
    files_under.clear();
    texts.clear();
}

disk_scope_t::disk_scope_t(disk_cache_t* cache) : m_previous(current) {
    if (cache == nullptr && current == nullptr) {
        m_own = std::make_unique<disk_cache_t>();
        cache = m_own.get();
    }

    if (cache != nullptr) {
        current = cache;
    }
}

disk_scope_t::~disk_scope_t() {
    current = m_previous;
}

namespace disk {

std::filesystem::path weakly_canonical(const std::filesystem::path& path, std::error_code& error) {
    auto [canonical, canonical_error] = remembered(&disk_cache_t::canonical, path.native(), [&] {
        std::error_code asked;
        std::filesystem::path found = std::filesystem::weakly_canonical(path, asked);
        return std::pair(std::move(found), asked);
    });
    error = canonical_error;
    return canonical;
}

std::optional<std::filesystem::path> real_path(const std::filesystem::path& path) {
    return remembered(&disk_cache_t::real, path.native(), [&] { return fb::real_path(path); });
}

std::filesystem::file_type type(const std::filesystem::path& path, std::error_code& error) {
    const disk_cache_t::status_t status = remembered(&disk_cache_t::statuses, path.native(), [&] {
        std::error_code asked;
        const std::filesystem::file_status found = std::filesystem::status(path, asked);
        return disk_cache_t::status_t{ .type = found.type(), .error = asked };
    });
    error = status.error;
    return status.type;
}

bool exists(const std::filesystem::path& path, std::error_code& error) {
    // As std::filesystem::exists(): a path that isn't there isn't an error.
    const std::filesystem::file_type found = type(path, error);
    if (found == std::filesystem::file_type::none) {
        return false;
    }

    error.clear();
    return found != std::filesystem::file_type::not_found;
}

bool is_regular_file(const std::filesystem::path& path, std::error_code& error) {
    return type(path, error) == std::filesystem::file_type::regular;
}

bool is_directory(const std::filesystem::path& path, std::error_code& error) {
    return type(path, error) == std::filesystem::file_type::directory;
}

bool equivalent(const std::filesystem::path& first, const std::filesystem::path& second,
                std::error_code& error) {
    auto [same, same_error] = remembered(&disk_cache_t::equivalent, pair_key(first, second), [&] {
        std::error_code asked;
        const bool found = std::filesystem::equivalent(first, second, asked);
        return std::pair(found, asked);
    });
    error = same_error;
    return same;
}

std::shared_ptr<const std::vector<disk_entry_t>> entries(const std::filesystem::path& folder,
                                                         std::error_code& error) {
    auto [found, found_error] = remembered(&disk_cache_t::entries, folder.native(), [&] {
        std::vector<disk_entry_t> listed;
        std::error_code asked;
        for (auto entry = std::filesystem::directory_iterator(folder, asked);
             !asked && entry != std::filesystem::directory_iterator(); entry.increment(asked)) {
            std::error_code ignored;
            listed.push_back({
                .path = entry->path(),
                .file = entry->is_regular_file(ignored),
                .folder = entry->is_directory(ignored),
            });
        }

        return std::pair(std::make_shared<const std::vector<disk_entry_t>>(std::move(listed)),
                         asked);
    });
    error = found_error;
    return found;
}

std::vector<std::filesystem::path> files_under(const std::filesystem::path& folder) {
    return remembered(&disk_cache_t::files_under, folder.native(), [&] {
        std::vector<std::filesystem::path> found;
        std::error_code error;
        for (auto entry = std::filesystem::recursive_directory_iterator(folder, error);
             !error && entry != std::filesystem::recursive_directory_iterator();
             entry.increment(error)) {
            std::error_code ignored;
            if (entry->is_regular_file(ignored)) {
                found.push_back(entry->path());
            }
        }

        return found;
    });
}

std::optional<std::string> read(const std::filesystem::path& path) {
    const std::shared_ptr<const std::optional<std::string>> text =
        remembered(&disk_cache_t::texts, path.native(), [&] {
            std::error_code error;
            if (!std::filesystem::is_regular_file(path, error)) {
                return std::make_shared<const std::optional<std::string>>();
            }

            const std::ifstream file(path, std::ios::binary);
            if (!file) {
                return std::make_shared<const std::optional<std::string>>();
            }

            std::ostringstream bytes;
            bytes << file.rdbuf();
            return std::make_shared<const std::optional<std::string>>(bytes.str());
        });
    return *text;
}

} // namespace disk

} // namespace fb
