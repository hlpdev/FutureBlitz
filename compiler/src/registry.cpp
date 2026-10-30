#include "fb/registry.hpp"

#include "fb/manifest.hpp"
#include "fb/package_archive.hpp"
#include "fb/semver.hpp"
#include "fb/utf8.hpp"

#include <algorithm>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <optional>
#include <random>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace fb {
namespace {

// Keys stay in the order they are written, as docs/registry-api.md writes an index line's.
using json_t = nlohmann::ordered_json;

/// A file's bytes, or nullopt when it can't be read.
std::optional<std::string> read_bytes(const std::filesystem::path& path) {
    const std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }

    std::ostringstream bytes;
    bytes << in.rdbuf();
    return std::move(bytes).str();
}

/// A name for a file beside `path`, which no other process picks.
std::filesystem::path partial_beside(const std::filesystem::path& path) {
    std::random_device random;
    std::filesystem::path partial = path;
    partial += std::format(".partial-{:08x}{:08x}", random(), random());
    return partial;
}

/// Writes `bytes` to `path` whole or not at all: to a file beside it, then renamed.
bool write_whole(const std::filesystem::path& path, const std::string_view bytes) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    const std::filesystem::path partial = partial_beside(path);
    {
        std::ofstream out(partial, std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.close();
        if (!out) {
            std::filesystem::remove(partial, error);
            return false;
        }
    }

    std::filesystem::rename(partial, path, error);
    if (error) {
        std::filesystem::remove(partial, error);
        return false;
    }

    return true;
}

/// Whether text is 64 lowercase hex digits.
bool is_sha256(const std::string_view text) {
    return text.size() == 64 && std::ranges::all_of(text, [](const char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

/// A string field of a JSON object, or nullopt.
std::optional<std::string> string_field(const json_t& object, const std::string_view key) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_string()) {
        return std::nullopt;
    }

    return found->get<std::string>();
}

/// A URL with a '/' at its end, so a name can go after it.
std::string with_slash(std::string url) {
    if (!url.ends_with('/')) {
        url += '/';
    }

    return url;
}

/// An archive's SHA-256 and size, from an index line's "archives", or nullopt.
std::optional<archive_ref_t> archive_of(const json_t& object) {
    if (!object.is_object()) {
        return std::nullopt;
    }

    const std::optional<std::string> sha256 = string_field(object, "sha256");
    const auto size = object.find("size");
    if (!sha256 || !is_sha256(*sha256) || size == object.end() || !size->is_number_unsigned()) {
        return std::nullopt;
    }

    return archive_ref_t{ .sha256 = *sha256, .size = size->get<std::uint64_t>() };
}

json_t json_of(const archive_ref_t& archive) {
    return { { "sha256", archive.sha256 }, { "size", archive.size } };
}

} // namespace

std::string api_error(const http_response_t& response) {
    const json_t body = json_t::parse(response.body, nullptr, false);
    const auto errors = body.is_object() ? body.find("errors") : body.end();
    if (body.is_object() && errors != body.end() && errors->is_array()) {
        std::string out;
        for (const json_t& error : *errors) {
            const std::optional<std::string> detail =
                error.is_object() ? string_field(error, "detail") : std::nullopt;
            if (detail) {
                out += std::format("{}{}", out.empty() ? "" : "; ", *detail);
            }
        }

        if (!out.empty()) {
            return out;
        }
    }

    return std::format("it answered {}", response.status);
}

std::expected<std::vector<index_entry_t>, std::string> parse_index(const std::string_view text,
                                                                   const std::string_view name) {
    std::vector<index_entry_t> entries;
    std::size_t number = 0;
    for (const auto part : std::views::split(text, '\n')) {
        ++number;
        std::string_view line(part.begin(), part.end());
        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }

        if (line.empty()) {
            continue;
        }

        const std::string wrong =
            std::format("line {} of {}'s index file isn't a version of it", number, name);
        const json_t object = json_t::parse(line, nullptr, false);
        if (!object.is_object()) {
            return std::unexpected(wrong);
        }

        // A line of a later format is for a later fb.
        if (const auto format = object.find("v"); format != object.end() &&
                                                  format->is_number_integer() &&
                                                  format->get<std::int64_t>() > index_format) {
            continue;
        }

        index_entry_t entry;
        const std::optional<std::string> named = string_field(object, "name");
        const std::optional<std::string> version = string_field(object, "version");
        const auto archives = object.find("archives");
        const std::optional<archive_ref_t> source =
            archives != object.end() && archives->is_object() && archives->contains("source")
                ? archive_of(archives->at("source"))
                : std::nullopt;
        if (named != name || !version || !parse_version(*version) || !source) {
            return std::unexpected(wrong);
        }

        entry.name = *named;
        entry.version = *version;
        entry.source = *source;
        entry.fb = string_field(object, "fb");
        if (const auto yanked = object.find("yanked"); yanked != object.end()) {
            entry.yanked = yanked->is_boolean() && yanked->get<bool>();
        }

        if (const auto dependencies = object.find("deps"); dependencies != object.end()) {
            if (!dependencies->is_array()) {
                return std::unexpected(wrong);
            }

            for (const json_t& each : *dependencies) {
                const std::optional<std::string> dependency =
                    each.is_object() ? string_field(each, "name") : std::nullopt;
                const std::optional<std::string> req =
                    each.is_object() ? string_field(each, "req") : std::nullopt;
                if (!dependency || !req) {
                    return std::unexpected(wrong);
                }

                entry.dependencies.push_back({
                    .name = *dependency,
                    .req = *req,
                    .registry = string_field(each, "registry"),
                });
            }
        }

        // Each target it has an archive of its own for, which "archives" has.
        if (const auto targets = object.find("targets"); targets != object.end()) {
            if (!targets->is_array()) {
                return std::unexpected(wrong);
            }

            for (const json_t& target : *targets) {
                // Only a target fb builds for, which a registry takes.
                const std::optional<archive_ref_t> archive =
                    target.is_string() &&
                            std::ranges::contains(package_targets, target.get<std::string>()) &&
                            archives->contains(target.get<std::string>())
                        ? archive_of(archives->at(target.get<std::string>()))
                        : std::nullopt;
                if (!archive) {
                    return std::unexpected(wrong);
                }

                entry.targets.emplace(target.get<std::string>(), *archive);
            }
        }

        std::ranges::sort(entry.dependencies, {}, &index_dependency_t::name);
        entries.push_back(std::move(entry));
    }

    return entries;
}

std::string index_line(const index_entry_t& entry) {
    std::vector<index_dependency_t> sorted = entry.dependencies;
    std::ranges::sort(sorted, {}, &index_dependency_t::name);
    json_t dependencies = json_t::array();
    for (const index_dependency_t& dependency : sorted) {
        json_t each = { { "name", dependency.name }, { "req", dependency.req } };
        if (dependency.registry) {
            each["registry"] = *dependency.registry;
        }

        dependencies.push_back(std::move(each));
    }

    json_t targets = json_t::array();
    json_t archives = { { "source", json_of(entry.source) } };
    for (const auto& [target, archive] : entry.targets) {
        targets.push_back(target);
        archives[target] = json_of(archive);
    }

    // In the order docs/registry-api.md writes them.
    json_t line = json_t::object();
    line["v"] = index_format;
    line["name"] = entry.name;
    line["version"] = entry.version;
    line["deps"] = std::move(dependencies);
    line["fb"] = entry.fb ? json_t(*entry.fb) : json_t(nullptr);
    line["targets"] = std::move(targets);
    line["archives"] = std::move(archives);
    line["yanked"] = entry.yanked;
    return line.dump();
}

std::expected<registry_config_t, std::string> parse_registry_config(const std::string_view text) {
    const json_t object = json_t::parse(text, nullptr, false);
    const std::optional<std::string> dl =
        object.is_object() ? string_field(object, "dl") : std::nullopt;
    if (!dl) {
        return std::unexpected("its config.json doesn't say where its archives are, as \"dl\"");
    }

    return registry_config_t{
        .dl = *dl,
        .api = string_field(object, "api").value_or(""),
        .oidc_audience = string_field(object, "oidc_audience").value_or(""),
    };
}

std::string download_url(const std::string_view dl, const std::string_view name,
                         const std::string_view version, const std::string_view file) {
    if (!dl.contains('{')) {
        return std::format("{}{}/{}/{}", with_slash(std::string(dl)), name, version, file);
    }

    std::string out(dl);
    const auto fill = [&](const std::string_view mark, const std::string_view value) {
        for (std::size_t at = out.find(mark); at != std::string::npos;
             at = out.find(mark, at + value.size())) {
            out.replace(at, mark.size(), value);
        }
    };
    fill("{name}", name);
    fill("{version}", version);
    fill("{file}", file);
    return out;
}

std::filesystem::path fb_home() {
#ifdef _WIN32
    const wchar_t* home = _wgetenv(L"FB_HOME");
    if (home != nullptr && *home != L'\0') {
        return { home };
    }

    const wchar_t* profile = _wgetenv(L"USERPROFILE");
    return std::filesystem::path(profile != nullptr ? profile : L".") / ".futureblitz";
#else
    const char* home = std::getenv("FB_HOME");
    if (home != nullptr && *home != '\0') {
        return { home };
    }

    const char* user = std::getenv("HOME");
    return std::filesystem::path(user != nullptr ? user : ".") / ".futureblitz";
#endif
}

std::string canonical_index(const std::string_view index, const std::filesystem::path& base) {
    if (index.starts_with("http://") || index.starts_with("https://")) {
        return with_slash(std::string(index));
    }

    std::filesystem::path folder;
    if (index.starts_with("file://")) {
        std::string_view rest = index.substr(std::string_view("file://").size());
        // file:///C:/registry is C:/registry.
        if (rest.size() > 2 && rest[0] == '/' && rest[2] == ':') {
            rest.remove_prefix(1);
        }

        folder = path_from_utf8(rest);
    } else {
        folder = base / path_from_utf8(index);
    }

    std::error_code error;
    folder = std::filesystem::weakly_canonical(std::filesystem::absolute(folder, error), error);
    std::string path = to_utf8(folder);
    std::ranges::replace(path, '\\', '/');
    while (path.size() > 1 && path.ends_with('/')) {
        path.pop_back();
    }

    return std::format("file://{}{}/", path.starts_with('/') ? "" : "/", path);
}

std::filesystem::path local_index_folder(const std::string_view canonical) {
    if (!canonical.starts_with("file://")) {
        return {};
    }

    std::string_view rest = canonical.substr(std::string_view("file://").size());
    if (rest.size() > 2 && rest[0] == '/' && rest[2] == ':') {
        rest.remove_prefix(1);
    }

    while (rest.size() > 1 && rest.ends_with('/')) {
        rest.remove_suffix(1);
    }

    return path_from_utf8(rest);
}

std::string registry_folder(const std::string_view canonical) {
    std::string_view name = canonical;
    if (const std::size_t scheme = name.find("://"); scheme != std::string_view::npos) {
        name = name.substr(scheme + 3);
    }

    // A URL's host, or a folder's own name.
    if (canonical.starts_with("file://")) {
        std::string_view path = name;
        while (path.ends_with('/')) {
            path.remove_suffix(1);
        }

        name = path.substr(path.rfind('/') + 1);
    } else {
        name = name.substr(0, name.find_first_of("/:?#"));
    }

    std::string safe;
    for (const char c : name) {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_';
        safe += plain ? c : '_';
    }

    if (safe.empty()) {
        safe = "registry";
    }

    return std::format("{}-{}", safe, sha256_hex(canonical).substr(0, 16));
}

std::filesystem::path package_cache_folder(const std::string_view source,
                                           const std::string_view name,
                                           const std::string_view version) {
    return fb_home() / "cache" / "packages" / path_from_utf8(registry_folder(source)) /
           path_from_utf8(std::format("{}-{}", name, version));
}

std::string archive_name(const std::string_view name, const std::string_view version,
                         const std::string_view target) {
    return target.empty() ? std::format("{}-{}.fbpkg", name, version)
                          : std::format("{}-{}-{}.fbpkg", name, version, target);
}

registry_t::registry_t(std::string canonical, http_client_t http, const bool offline,
                       status_t status)
    : m_index(std::move(canonical)), m_folder(local_index_folder(m_index)), m_http(std::move(http)),
      m_offline(offline), m_status(std::move(status)) {}

namespace {

/// fb's cache folder.
std::filesystem::path cache_folder() {
    return fb_home() / "cache";
}

} // namespace

std::expected<std::vector<index_entry_t>, std::string>
registry_t::versions(const std::string_view name) {
    if (local()) {
        const std::filesystem::path file = m_folder / path_from_utf8(name);
        std::error_code error;
        // A folder that isn't a registry's is that, not a registry without the package.
        if (!std::filesystem::is_regular_file(m_folder / "config.json", error)) {
            return std::unexpected(
                std::format("there's no registry in {}: it has no config.json", to_utf8(m_folder)));
        }

        if (!std::filesystem::exists(file, error)) {
            return std::vector<index_entry_t>{};
        }

        const std::optional<std::string> text = read_bytes(file);
        if (!text) {
            return std::unexpected(std::format("couldn't read {}", to_utf8(file)));
        }

        return parse_index(*text, name);
    }

    const std::filesystem::path folder =
        cache_folder() / "index" / path_from_utf8(registry_folder(m_index));
    const std::filesystem::path cached = folder / path_from_utf8(name);
    std::filesystem::path etag_file = cached;
    etag_file += ".etag";
    const std::optional<std::string> kept = read_bytes(cached);
    const auto from_cache =
        [&](const std::string_view why) -> std::expected<std::vector<index_entry_t>, std::string> {
        if (!kept) {
            return std::unexpected(std::format(
                "{}, and the cache has no index of {}; without --offline, fb fetches it", why,
                name));
        }

        m_notes.push_back(std::format("{}, so {}'s versions are the cache's", why, name));
        return parse_index(*kept, name);
    };

    if (m_offline) {
        if (!kept) {
            return std::unexpected(std::format(
                "fb is offline, and the cache has no index of {}; without --offline, fb fetches "
                "it",
                name));
        }

        return parse_index(*kept, name);
    }

    if (m_unreachable) {
        return from_cache(std::format("{} couldn't be reached", m_index));
    }

    http_request_t request{ .url = m_index + std::string(name) };
    if (const std::optional<std::string> etag = read_bytes(etag_file); etag && kept) {
        request.headers.emplace_back("If-None-Match", *etag);
    }

    const std::expected<http_response_t, std::string> response = m_http(request);
    if (!response) {
        m_unreachable = true;
        return from_cache(response.error());
    }

    if (response->status == 304 && kept) {
        return parse_index(*kept, name);
    }

    if (response->status == 404 || response->status == 410) {
        return std::vector<index_entry_t>{};
    }

    if (response->status != 200) {
        return std::unexpected(std::format("{}'s index file couldn't be fetched from {}: {}", name,
                                           m_index, api_error(*response)));
    }

    std::expected<std::vector<index_entry_t>, std::string> entries =
        parse_index(response->body, name);
    if (entries) {
        // An index file that couldn't be kept is fetched again next time.
        std::error_code error;
        if (write_whole(cached, response->body)) {
            const auto etag = response->headers.find("etag");
            if (etag != response->headers.end()) {
                (void)write_whole(etag_file, etag->second);
            } else {
                std::filesystem::remove(etag_file, error);
            }
        }
    }

    return entries;
}

std::expected<registry_config_t, std::string> registry_t::config() {
    if (m_config) {
        return *m_config;
    }

    std::expected<registry_config_t, std::string> config =
        std::unexpected(std::string("no config.json"));
    if (local()) {
        const std::optional<std::string> text = read_bytes(m_folder / "config.json");
        if (!text) {
            return std::unexpected(
                std::format("the registry in {} has no config.json", to_utf8(m_folder)));
        }

        config = parse_registry_config(*text);
        if (config && !config->dl.starts_with("file://")) {
            // A folder from the index's own.
            config->dl = to_utf8(m_folder / path_from_utf8(config->dl));
        }
    } else {
        const std::filesystem::path cached =
            cache_folder() / "index" / path_from_utf8(registry_folder(m_index)) / "config.json";
        const std::optional<std::string> kept = read_bytes(cached);
        std::expected<http_response_t, std::string> response =
            std::unexpected(std::string("fb is offline"));
        if (!m_offline && !m_unreachable) {
            response = m_http({ .url = m_index + "config.json" });
        }

        if (response && response->status == 200) {
            config = parse_registry_config(response->body);
            if (config) {
                (void)write_whole(cached, response->body);
            }
        } else if (kept) {
            if (!response) {
                m_unreachable = !m_offline;
            }

            config = parse_registry_config(*kept);
        } else if (!response) {
            m_unreachable = !m_offline;
            return std::unexpected(std::format("{}, and the cache has no config.json of {}",
                                               response.error(), m_index));
        } else {
            return std::unexpected(std::format("{}config.json couldn't be fetched: {}", m_index,
                                               api_error(*response)));
        }
    }

    if (!config) {
        return std::unexpected(
            std::format("the registry at {} is broken: {}", m_index, config.error()));
    }

    // What's downloaded comes over TLS, or from this machine, as the index does.
    if (!local() && !config->dl.starts_with("https://") && !is_loopback_url(config->dl)) {
        return std::unexpected(
            std::format("the registry at {} has its archives at {}, which isn't https://", m_index,
                        config->dl));
    }

    m_config = *config;
    return *config;
}

std::expected<std::filesystem::path, std::string>
registry_t::archive(const std::string_view name, const std::string_view version,
                    const std::string_view file, const std::string_view checksum,
                    const std::uint64_t most) {
    std::filesystem::path cached = cache_folder() / "archives" /
                                   path_from_utf8(registry_folder(m_index)) / path_from_utf8(file);
    std::error_code error;
    if (std::filesystem::is_regular_file(cached, error) && sha256_file(cached) == checksum) {
        return cached;
    }

    const std::expected<registry_config_t, std::string> config = this->config();
    if (!config) {
        return std::unexpected(config.error());
    }

    const std::string url = download_url(config->dl, name, version, file);
    std::filesystem::path path;
    if (local()) {
        // A registry on disk's archive is used where it is.
        path = url.starts_with("file://") ? local_index_folder(url) : path_from_utf8(url);
        if (!std::filesystem::is_regular_file(path, error)) {
            return std::unexpected(std::format("couldn't read {}", to_utf8(path)));
        }

        if (std::filesystem::file_size(path, error) > most) {
            return std::unexpected(
                std::format("{} is more than the {} bytes fb takes of it", to_utf8(path), most));
        }
    } else {
        if (m_offline) {
            return std::unexpected(std::format(
                "fb is offline, and the cache doesn't have {}; without --offline, fb downloads it",
                file));
        }

        if (m_status) {
            m_status("Downloading", std::format("{} v{}", name, version));
        }

        // Into a file beside its place, as it comes, then renamed once it's checked.
        std::filesystem::create_directories(cached.parent_path(), error);
        path = partial_beside(cached);
        const std::expected<http_response_t, std::string> response =
            m_http({ .url = url, .save_to = path, .most_body = most });
        if (!response) {
            std::filesystem::remove(path, error);
            return std::unexpected(response.error());
        }

        if (response->status != 200) {
            std::filesystem::remove(path, error);
            return std::unexpected(
                std::format("{} couldn't be downloaded: {}", url, api_error(*response)));
        }

        // A client that kept the body itself, as a test's may, has it written here.
        if (!std::filesystem::exists(path, error) && !write_whole(path, response->body)) {
            return std::unexpected(std::format("couldn't write {}", to_utf8(path)));
        }
    }

    if (const std::string hash = sha256_file(path); hash != checksum) {
        if (!local()) {
            std::filesystem::remove(path, error);
        }

        return std::unexpected(
            std::format("{} isn't what fb.lock says it is: its SHA-256 is {}, and fb.lock's is {}",
                        file, hash, checksum));
    }

    if (local()) {
        return path;
    }

    std::filesystem::rename(path, cached, error);
    if (error) {
        std::filesystem::remove(path, error);
        return std::unexpected(std::format("couldn't write {}", to_utf8(cached)));
    }

    return cached;
}

} // namespace fb
