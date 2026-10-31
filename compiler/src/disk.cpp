#include "fb/disk.hpp"

#include "fb/process.hpp"

#include <algorithm>
#include <cctype>
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

/// What `ask` gives for `key`, about `path`, from the cache in use, which asks once about a path
/// it keeps answers about.
template <typename Value, typename Ask>
Value remembered(std::unordered_map<disk_cache_t::key_t, Value> disk_cache_t::* answers,
                 const disk_cache_t::key_t& key, const std::filesystem::path& path, Ask ask) {
    if (current == nullptr || !current->keeps(path)) {
        return ask();
    }

    return current->remember(answers, key, std::move(ask));
}

/// How keeps() compares a path: absolute, without "." or "..", with '/'s, and in one letter case
/// where the disk ignores it.
std::string comparable(const std::filesystem::path& path) {
    std::string text = path.lexically_normal().generic_string();
#if defined(_WIN32) || defined(__APPLE__)
    std::ranges::transform(text, text.begin(), [](const unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
#endif
    while (text.size() > 1 && text.ends_with('/')) {
        text.pop_back();
    }

    return text;
}

/// Where a path really leads, through the links and junctions on its way, as fb::real_path() has
/// it, from the nearest folder above it that's there when it isn't.
std::optional<std::filesystem::path> leads_to(const std::filesystem::path& path) {
    std::filesystem::path rest;
    for (std::filesystem::path at = path.lexically_normal(); !at.empty(); at = at.parent_path()) {
        if (std::optional<std::filesystem::path> real = fb::real_path(at)) {
            return rest.empty() ? *real : *real / rest;
        }

        rest = rest.empty() ? at.filename() : at.filename() / rest;
        if (at == at.parent_path()) {
            break;
        }
    }

    return std::nullopt;
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

void disk_cache_t::keep_only_under(const std::vector<std::filesystem::path>& folders) {
    // Each as it's named, and where it really is, which what's in it leads to too.
    std::vector<std::string> compared;
    compared.reserve(2 * folders.size());
    for (const std::filesystem::path& folder : folders) {
        compared.push_back(comparable(folder));
        if (const std::optional<std::filesystem::path> really = leads_to(folder)) {
            compared.push_back(comparable(*really));
        }
    }

    clear();
    const std::scoped_lock lock(mutex);
    watched = std::move(compared);
}

bool disk_cache_t::keeps(const std::filesystem::path& path) {
    std::vector<std::string> folders;
    std::uint64_t asked_at = 0;
    {
        const std::scoped_lock lock(mutex);
        if (!watched) {
            return true;
        }

        if (!path.is_absolute()) {
            return false;
        }

        if (const auto found = kept.find(path.native()); found != kept.end()) {
            return found->second;
        }

        folders = *watched;
        asked_at = clears;
    }

    const auto under = [&](const std::string& compared) {
        return std::ranges::any_of(folders, [&](const std::string& folder) {
            return compared == folder ||
                   (compared.starts_with(folder) && compared[folder.size()] == '/');
        });
    };
    // Where it leads, asked without the lock, as the disk is slow.
    bool inside = under(comparable(path));
    if (inside) {
        const std::optional<std::filesystem::path> really = leads_to(path);
        inside = really && under(comparable(*really));
    }

    const std::scoped_lock lock(mutex);
    if (clears == asked_at) {
        kept.try_emplace(path.native(), inside);
    }

    return inside;
}

void disk_cache_t::clear() {
    const std::scoped_lock lock(mutex);
    ++clears;
    kept.clear();
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
    const auto ask = [&] {
        std::error_code asked;
        std::filesystem::path found = std::filesystem::weakly_canonical(path, asked);
        return std::pair(std::move(found), asked);
    };
    if (current == nullptr || !current->keeps(path)) {
        auto [canonical, canonical_error] = ask();
        error = canonical_error;
        return canonical;
    }

    // Not where a link leads out of the folders it keeps answers about.
    auto [canonical, canonical_error] =
        current->remember(&disk_cache_t::canonical, path.native(), ask,
                          [](const std::pair<std::filesystem::path, std::error_code>& found) {
                              return current->keeps(found.first);
                          });
    error = canonical_error;
    return canonical;
}

std::optional<std::filesystem::path> real_path(const std::filesystem::path& path) {
    if (current == nullptr || !current->keeps(path)) {
        return fb::real_path(path);
    }

    return current->remember(
        &disk_cache_t::real, path.native(), [&] { return fb::real_path(path); },
        [](const std::optional<std::filesystem::path>& found) {
            return !found || current->keeps(*found);
        });
}

std::filesystem::file_type type(const std::filesystem::path& path, std::error_code& error) {
    const disk_cache_t::status_t status =
        remembered(&disk_cache_t::statuses, path.native(), path, [&] {
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
    // Kept only when both are where it keeps answers about.
    auto [same, same_error] =
        remembered(&disk_cache_t::equivalent, pair_key(first, second),
                   current != nullptr && !current->keeps(second) ? second : first, [&] {
                       std::error_code asked;
                       const bool found = std::filesystem::equivalent(first, second, asked);
                       return std::pair(found, asked);
                   });
    error = same_error;
    return same;
}

std::shared_ptr<const std::vector<disk_entry_t>> entries(const std::filesystem::path& folder,
                                                         std::error_code& error) {
    auto [found, found_error] = remembered(&disk_cache_t::entries, folder.native(), folder, [&] {
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
    return remembered(&disk_cache_t::files_under, folder.native(), folder, [&] {
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
        remembered(&disk_cache_t::texts, path.native(), path, [&] {
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

void forget() {
    if (current != nullptr) {
        current->clear();
    }
}

} // namespace disk

} // namespace fb
