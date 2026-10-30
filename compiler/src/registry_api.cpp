// What fb asks a registry to do: publish, yank, owners, search, and sign-in, as
// docs/registry-api.md has the API, and for a registry on disk, publishing into its folders.

#include "fb/registry_api.hpp"

#include "fb/diagnostic.hpp"
#include "fb/interface.hpp"
#include "fb/package_archive.hpp"
#include "fb/packages.hpp"
#include "fb/semver.hpp"
#include "fb/source.hpp"
#include "fb/utf8.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
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

using json_t = nlohmann::json;

/// The most a readme can be, and the most a description.
constexpr std::uint64_t most_readme_bytes = std::uint64_t{ 1 } << 20U;
/// The most a project.toml can be.
constexpr std::uint64_t most_manifest_bytes = std::uint64_t{ 1 } << 20U;
constexpr std::size_t longest_description = 300;
constexpr std::size_t longest_name = 64;

std::optional<std::string> read_bytes(const std::filesystem::path& path) {
    const std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }

    std::ostringstream bytes;
    bytes << in.rdbuf();
    return std::move(bytes).str();
}

bool write_bytes(const std::filesystem::path& path, const std::string_view bytes) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream out(path, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    return static_cast<bool>(out);
}

bool is_utf8(const std::string_view text) {
    for (std::size_t at = 0; at < text.size();) {
        const decoded_t decoded = decode_utf8(text, at);
        if (!decoded.valid()) {
            return false;
        }

        at += decoded.length;
    }

    return true;
}

/// The first of `names` that's a file in `root`, by its path there.
std::optional<std::string> first_file(const std::filesystem::path& root,
                                      const std::initializer_list<std::string_view> names) {
    std::error_code error;
    for (const std::string_view name : names) {
        if (std::filesystem::is_regular_file(root / path_from_utf8(name), error)) {
            return std::string(name);
        }
    }

    return std::nullopt;
}

/// An archive of `paths` from `root`, named `file`.
/// A size in the registry's megabytes, which are a million bytes: "10 MB", or "10.5 MB".
std::string megabytes(const std::uint64_t bytes) {
    if (bytes % 1'000'000 == 0) {
        return std::format("{} MB", bytes / 1'000'000);
    }

    return std::format("{:.1f} MB", static_cast<double>(bytes) / 1e6);
}

/// The archive `file` of `paths`, from `root`, refused as fb would refuse to unpack it: with
/// more than the registry's files, or unpacking to more than `unpack_factor` times the larger of
/// `most`, the registry's limit on its kind, and its own size.
std::expected<built_archive_t, std::string> build_archive(const std::filesystem::path& root,
                                                          std::string file,
                                                          const std::vector<std::string>& paths,
                                                          const std::uint64_t most) {
    if (paths.size() > most_archive_files) {
        return std::unexpected(std::format("{} would have {} files, more than the registry's {}",
                                           file, paths.size(), most_archive_files));
    }

    std::vector<archive_file_t> files;
    std::uint64_t unpacked = 0;
    for (const std::string& path : paths) {
        std::optional<std::string> bytes = read_bytes(root / path_from_utf8(path));
        if (!bytes) {
            return std::unexpected(std::format("couldn't read {}", path));
        }

        unpacked += bytes->size();
        files.push_back({ .path = path, .bytes = std::move(*bytes) });
    }

    std::expected<std::string, std::string> bytes = write_archive(std::move(files));
    if (!bytes) {
        return std::unexpected(std::format("{} can't be archived: {}", file, bytes.error()));
    }

    const std::uint64_t most_unpacked =
        unpack_factor * std::max<std::uint64_t>(most, bytes->size());
    if (unpacked > most_unpacked) {
        return std::unexpected(
            std::format("{} unpacks to {}, more than the {} fb unpacks of an archive of its kind",
                        file, megabytes(unpacked), megabytes(most_unpacked)));
    }

    built_archive_t archive{ .file = std::move(file), .bytes = std::move(*bytes), .paths = paths };
    archive.ref = { .sha256 = sha256_hex(archive.bytes), .size = archive.bytes.size() };
    return archive;
}

/// Checks a prebuilt target's builds: debug's and release's .fblib and .fbi, each naming the
/// package, its version, and one ABI hash.
std::expected<void, std::string> check_prebuilt(const std::filesystem::path& root,
                                                const manifest_t& manifest,
                                                const std::string_view target) {
    for (const std::string_view mode : { "debug", "release" }) {
        const std::string folder = std::format("lib/{}/{}/", target, mode);
        const std::optional<std::string> interface =
            read_bytes(root / path_from_utf8(folder + manifest.name + ".fbi"));
        const std::optional<std::string> binary =
            read_bytes(root / path_from_utf8(folder + manifest.name + ".fblib"));
        if (!interface || !binary) {
            return std::unexpected(std::format(
                "{} is prebuilt for {}, so it needs {}{}.fbi and {}{}.fblib, a debug and a release "
                "build of each, which fb build and fb build --release write",
                manifest.name, target, folder, manifest.name, folder, manifest.name));
        }

        const std::optional<interface_header_t> header = read_interface_header(*interface);
        const std::optional<library_binary_t> code = read_binary(*binary);
        if (!header || !code) {
            return std::unexpected(
                std::format("{}{}.fbi or .fblib isn't one fb writes", folder, manifest.name));
        }

        if (header->name != manifest.name || header->version != manifest.version ||
            code->header.name != manifest.name || code->header.version != manifest.version ||
            header->hash != code->header.hash) {
            return std::unexpected(std::format(
                "{}{}.fbi is {} v{} ({}), and its .fblib is {} v{} ({}), but they have to be one "
                "build of {} v{}",
                folder, manifest.name, header->name, header->version, header->hash,
                code->header.name, code->header.version, code->header.hash, manifest.name,
                manifest.version));
        }
    }

    return {};
}

/// The registry's API's URL, from its config.json.
std::expected<std::string, std::string> api_of(const registry_session_t& session) {
    // Fetched once a session.
    if (!session.api.empty()) {
        return session.api;
    }

    registry_t registry(session.index, session.http, false, session.status);
    const std::expected<registry_config_t, std::string> config = registry.config();
    if (!config) {
        return std::unexpected(config.error());
    }

    if (config->api.empty()) {
        return std::unexpected(
            std::format("the registry at {} has no API in its config.json", session.index));
    }

    // Tokens go only where TLS keeps them, or to this machine.
    if (!config->api.starts_with("https://") && !is_loopback_url(config->api)) {
        return std::unexpected(std::format(
            "the registry at {} names {} as its API, which isn't https://, so fb sends it no "
            "token",
            session.index, config->api));
    }

    std::string api = config->api;
    while (api.ends_with('/')) {
        api.pop_back();
    }

    session.api = api;
    return api;
}

/// Sends a request to the API, at `path` after its URL, with the session's token, and JSON when
/// `body` isn't null. Fails, with the registry's words, for a status that isn't `expect`'s.
std::expected<http_response_t, std::string> call(const registry_session_t& session,
                                                 const std::string_view method,
                                                 const std::string_view path, const json_t& body,
                                                 const std::initializer_list<long> expect) {
    const std::expected<std::string, std::string> api = api_of(session);
    if (!api) {
        return std::unexpected(api.error());
    }

    http_request_t request{ .method = std::string(method), .url = *api + std::string(path) };
    request.headers.emplace_back("Accept", "application/json");
    if (!session.token.empty()) {
        request.headers.emplace_back("Authorization", std::format("Bearer {}", session.token));
    }

    if (!body.is_null()) {
        request.headers.emplace_back("Content-Type", "application/json");
        request.body = body.dump();
    }

    std::expected<http_response_t, std::string> response = session.http(request);
    if (!response) {
        return std::unexpected(response.error());
    }

    if (!std::ranges::contains(expect, response->status)) {
        return std::unexpected(api_error(*response));
    }

    return response;
}

/// Uploads `bytes`, by its SHA-256, unless the registry has it staged already: in one request,
/// or in parts when it's over 64 MB.
std::expected<void, std::string> upload(const registry_session_t& session,
                                        const std::string_view what, const std::string_view bytes,
                                        const archive_ref_t& ref) {
    const std::expected<std::string, std::string> api = api_of(session);
    if (!api) {
        return std::unexpected(api.error());
    }

    const std::string url = std::format("{}/uploads/{}", *api, ref.sha256);
    const std::vector<std::pair<std::string, std::string>> auth{
        { "Authorization", std::format("Bearer {}", session.token) },
    };
    const std::expected<http_response_t, std::string> staged =
        session.http({ .method = "HEAD", .url = url, .headers = auth });
    if (!staged) {
        return std::unexpected(staged.error());
    }

    if (staged->status == 200) {
        return {};
    }

    if (session.status) {
        session.status("Uploading", std::format("{} ({} KB)", what, (ref.size + 1023) / 1024));
    }

    if (ref.size <= session.most_single_upload) {
        http_request_t request{
            .method = "PUT",
            .url = url,
            .headers = auth,
            .upload = bytes,
        };
        request.headers.emplace_back("Content-Type", "application/octet-stream");
        const std::expected<http_response_t, std::string> put = session.http(request);
        if (!put) {
            return std::unexpected(put.error());
        }

        if (put->status != 201 && put->status != 200) {
            return std::unexpected(
                std::format("{} couldn't be uploaded: {}", what, api_error(*put)));
        }

        return {};
    }

    const std::expected<http_response_t, std::string> started =
        call(session, "POST", std::format("/uploads/{}/parts", ref.sha256),
             { { "size", ref.size } }, { 201, 200 });
    if (!started) {
        return std::unexpected(std::format("{} couldn't be uploaded: {}", what, started.error()));
    }

    const json_t answer = json_t::parse(started->body, nullptr, false);
    if (!answer.is_object() || !answer.contains("upload_id") || !answer["upload_id"].is_string() ||
        !answer.contains("part_size") || !answer["part_size"].is_number_unsigned() ||
        answer["part_size"].get<std::uint64_t>() == 0) {
        return std::unexpected(std::format("{} couldn't be uploaded: the registry's answer "
                                           "had no upload_id and part_size",
                                           what));
    }

    const std::string id = answer["upload_id"].get<std::string>();
    const std::uint64_t part_size = answer["part_size"].get<std::uint64_t>();
    json_t parts = json_t::array();
    std::size_t number = 1;
    for (std::uint64_t at = 0; at < bytes.size(); at += part_size, ++number) {
        http_request_t request{
            .method = "PUT",
            .url = std::format("{}/uploads/{}/parts/{}/{}", *api, ref.sha256, id, number),
            .headers = auth,
            .upload = bytes.substr(at, part_size),
        };
        request.headers.emplace_back("Content-Type", "application/octet-stream");
        const std::expected<http_response_t, std::string> put = session.http(request);
        if (!put) {
            return std::unexpected(put.error());
        }

        const json_t part = json_t::parse(put->body, nullptr, false);
        if (put->status != 200 || !part.is_object() || !part.contains("etag")) {
            return std::unexpected(std::format("part {} of {} couldn't be uploaded: {}", number,
                                               what, api_error(*put)));
        }

        parts.push_back({ { "part", number }, { "etag", part["etag"] } });
    }

    const std::expected<http_response_t, std::string> done =
        call(session, "POST", std::format("/uploads/{}/parts/{}/complete", ref.sha256, id),
             { { "parts", parts } }, { 201, 200 });
    if (!done) {
        return std::unexpected(std::format("{} couldn't be uploaded: {}", what, done.error()));
    }

    return {};
}

/// Publishes into a registry on disk: the archives into its `dl`, and the line into its index.
std::expected<void, std::string> publish_locally(const prepared_package_t& package,
                                                 const registry_session_t& session) {
    registry_t registry(session.index, session.http, false, session.status);
    const std::expected<registry_config_t, std::string> config = registry.config();
    if (!config) {
        return std::unexpected(config.error());
    }

    const std::filesystem::path folder = local_index_folder(session.index);
    const std::string& name = package.manifest.name;
    const std::string& version = package.manifest.version;
    std::vector<const built_archive_t*> archives{ &package.source };
    for (const built_archive_t& target : package.targets) {
        archives.push_back(&target);
    }

    for (const built_archive_t* archive : archives) {
        const std::string url = download_url(config->dl, name, version, archive->file);
        const std::filesystem::path path =
            url.starts_with("file://") ? local_index_folder(url) : path_from_utf8(url);
        if (!write_bytes(path, archive->bytes)) {
            return std::unexpected(std::format("couldn't write {}", to_utf8(path)));
        }
    }

    const std::filesystem::path index = folder / path_from_utf8(name);
    std::string text = read_bytes(index).value_or("");
    if (!text.empty() && !text.ends_with('\n')) {
        text += '\n';
    }

    text += index_line(package.entry) + "\n";
    if (!write_bytes(index, text)) {
        return std::unexpected(std::format("couldn't write {}", to_utf8(index)));
    }

    return {};
}

/// Changes one version's line in a registry on disk's index.
std::expected<void, std::string> yank_locally(const std::string_view name,
                                              const std::string_view version, const bool yanked,
                                              const registry_session_t& session) {
    const std::filesystem::path index = local_index_folder(session.index) / path_from_utf8(name);
    const std::optional<std::string> text = read_bytes(index);
    const std::expected<std::vector<index_entry_t>, std::string> entries =
        text ? parse_index(*text, name) : std::vector<index_entry_t>{};
    if (!entries) {
        return std::unexpected(entries.error());
    }

    std::string out;
    bool found = false;
    for (index_entry_t entry : *entries) {
        if (entry.version == version) {
            found = true;
            entry.yanked = yanked;
        }

        out += index_line(entry) + "\n";
    }

    if (!found) {
        return std::unexpected(std::format("{} has no version {}", name, version));
    }

    if (!write_bytes(index, out)) {
        return std::unexpected(std::format("couldn't write {}", to_utf8(index)));
    }

    return {};
}

std::string text_field(const json_t& object, const std::string_view key) {
    const auto found = object.find(key);
    return found != object.end() && found->is_string() ? found->get<std::string>() : "";
}

} // namespace

namespace {

/// Whether `bare`, a name without its '_'s, is one the registry keeps, as docs/registry-api.md
/// has them.
constexpr bool is_reserved(const std::string_view bare) {
    return bare == "std" || bare == "fb" || bare == "futureblitz" || bare == "apt" ||
           bare == "rpm" || bare == "releases";
}

} // namespace

std::optional<std::string> registry_name_error(const std::string_view name) {
    const bool shaped = !name.empty() && name.size() <= longest_name && name.front() >= 'a' &&
                        name.front() <= 'z' && std::ranges::all_of(name, [](const char c) {
                            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
                        });
    if (!shaped) {
        return std::format("a registry's package is named with lowercase letters, digits, and "
                           "'_', starting with a letter, at most {} long, not \"{}\"",
                           longest_name, name);
    }

    std::string bare(name);
    std::erase(bare, '_');
    // FutureBlitz's own folders on the registry's downloads are kept too: apt's and dnf's
    // repositories, and fb's releases.
    if (is_reserved(bare)) {
        return std::format("{} is a name the registry keeps for FutureBlitz itself", name);
    }

    return std::nullopt;
}

std::optional<std::string> target_size_error(const std::string_view file, const std::uint64_t size,
                                             const bool on_disk) {
    // A registry on the network raises its limit for a package that asks, and knows each
    // package's, so it decides.
    if (!on_disk || size <= most_target_archive) {
        return std::nullopt;
    }

    return std::format("{} is {}, more than a registry on disk's {}", file, megabytes(size),
                       megabytes(most_target_archive));
}

std::expected<prepared_package_t, std::string> prepare_package(const std::filesystem::path& root,
                                                               const std::string_view index,
                                                               const bool public_registry) {
    const bool on_disk = is_local_index(index);
    const std::optional<std::string> text = read_bytes(root / "project.toml");
    if (!text) {
        return std::unexpected(std::format("there's no project.toml in {}", to_utf8(root)));
    }

    if (text->size() > most_manifest_bytes) {
        return std::unexpected("project.toml is more than the registry's 1 MB");
    }

    const source_file_t file(file_id_t{}, "project.toml", *text);
    diagnostic_bag_t diagnostics;
    std::optional<manifest_t> read = read_manifest(file, diagnostics);
    if (!read) {
        return std::unexpected("project.toml has mistakes, which fb check shows");
    }

    prepared_package_t package{ .manifest = std::move(*read) };
    const manifest_t& manifest = package.manifest;
    if (const std::optional<std::string> wrong = registry_name_error(manifest.name)) {
        return std::unexpected(*wrong);
    }

    std::error_code error;
    const bool has_main =
        std::filesystem::is_regular_file(root / path_from_utf8(manifest.main), error);
    if (manifest.type == package_type_t::EXECUTABLE ||
        (!manifest.type && (manifest.main_span || has_main))) {
        return std::unexpected(std::format(
            "{} is an executable, and only a library can be published, since programs use "
            "libraries; a library is type = \"library\" in [project]",
            manifest.name));
    }

    if (!manifest.description || manifest.description->empty()) {
        return std::unexpected(
            "fb publish needs a description in [project], a line about the package, like "
            "description = \"A 2D physics engine\"");
    }

    // A line, of characters as the registry counts them, in UTF-16: a code point past U+FFFF is
    // two.
    std::size_t characters = 0;
    for (std::size_t at = 0; at < manifest.description->size();) {
        const decoded_t decoded = decode_utf8(*manifest.description, at);
        characters += decoded.code_point > 0xFFFF ? 2 : 1;
        at += decoded.length;
    }

    if (characters > longest_description) {
        return std::unexpected(std::format("[project]'s description is longer than {} characters",
                                           longest_description));
    }

    if (manifest.description->find_first_of("\r\n") != std::string::npos) {
        return std::unexpected("[project]'s description is a line, without line breaks");
    }

    if (!manifest.license) {
        return std::unexpected("fb publish needs [project]'s license, an SPDX expression like "
                               "license = \"MIT\" or \"MIT OR Apache-2.0\"");
    }

    const std::optional<std::string> license_file =
        manifest.license_file
            ? first_file(root, { *manifest.license_file })
            : first_file(root, { "LICENSE", "LICENSE.md", "LICENSE.txt", "COPYING" });
    if (!license_file) {
        return std::unexpected(manifest.license_file
                                   ? std::format("there's no {}, which [project]'s license_file "
                                                 "names",
                                                 *manifest.license_file)
                                   : std::string("fb publish needs the license's text in the "
                                                 "package's folder: LICENSE, LICENSE.md, "
                                                 "LICENSE.txt, or COPYING, or the file [project]'s "
                                                 "license_file names"));
    }

    const std::optional<std::string> readme =
        manifest.readme ? first_file(root, { *manifest.readme })
                        : first_file(root, { "README.md", "README", "README.txt" });
    if (!readme) {
        return std::unexpected(
            manifest.readme
                ? std::format("there's no {}, which [project]'s readme names", *manifest.readme)
                : std::string("fb publish needs a readme in the package's "
                              "folder: README.md, README, or README.txt, or "
                              "the file [project]'s readme names"));
    }

    const std::optional<std::string> readme_text = read_bytes(root / path_from_utf8(*readme));
    if (!readme_text || readme_text->size() > most_readme_bytes || !is_utf8(*readme_text)) {
        return std::unexpected(std::format("{} has to be UTF-8, and at most 1 MB", *readme));
    }

    package.entry = { .name = manifest.name, .version = manifest.version };
    if (manifest.fb) {
        package.entry.fb = manifest.fb->text;
    }

    for (const dependency_t& dependency : manifest.dependencies) {
        if (!dependency.range) {
            return std::unexpected(std::format(
                "{} is a path, {} = {{ path = \"{}\" }}, but a published package's dependencies "
                "are from a registry, by their versions, like {} = \"1.2\"",
                dependency.name, dependency.name, dependency.path, dependency.name));
        }

        if (const std::optional<std::string> wrong = registry_name_error(dependency.name)) {
            return std::unexpected(*wrong);
        }

        if (dependency.name == manifest.name) {
            return std::unexpected(std::format("{} can't depend on itself", manifest.name));
        }

        index_dependency_t entry{ .name = dependency.name, .req = dependency.range->text };
        const std::optional<std::string> source =
            registry_index(dependency.registry, manifest.registries, root);
        if (source && *source != index) {
            if (public_registry) {
                return std::unexpected(std::format(
                    "{} is from another registry, but futureblitz.dev's packages depend only on "
                    "its own",
                    dependency.name));
            }

            entry.registry = *source;
        }

        package.entry.dependencies.push_back(std::move(entry));
    }

    std::ranges::sort(package.entry.dependencies, {}, &index_dependency_t::name);
    const package_files_t files = package_files(root);
    const bool source_code = std::ranges::any_of(
        files.source, [](const std::string& path) { return path.ends_with(".fb"); });
    std::vector<std::string> prebuilt;
    for (const auto& [target, paths] : files.targets) {
        if (std::ranges::any_of(paths, [&](const std::string& path) {
                return path.starts_with(std::format("lib/{}/", target));
            })) {
            prebuilt.push_back(target);
        }
    }

    if (source_code && !prebuilt.empty()) {
        return std::unexpected(std::format(
            "{} has both .fb source and a prebuilt lib/{}/, but a package is one or the other",
            manifest.name, prebuilt.front()));
    }

    if (!source_code && files.targets.empty()) {
        return std::unexpected(std::format(
            "{} has no .fb source and no prebuilt lib/<target>/, so there's nothing to publish",
            manifest.name));
    }

    for (const std::string& target : prebuilt) {
        if (const std::expected<void, std::string> checked = check_prebuilt(root, manifest, target);
            !checked) {
            return std::unexpected(checked.error());
        }
    }

    std::expected<built_archive_t, std::string> source = build_archive(
        root, archive_name(manifest.name, manifest.version), files.source, most_source_archive);
    if (!source) {
        return std::unexpected(source.error());
    }

    if (source->ref.size > most_source_archive) {
        return std::unexpected(std::format("{} is {}, more than the registry's {}", source->file,
                                           megabytes(source->ref.size),
                                           megabytes(most_source_archive)));
    }

    package.source = std::move(*source);
    package.entry.source = package.source.ref;
    for (const auto& [target, paths] : files.targets) {
        std::expected<built_archive_t, std::string> archive =
            build_archive(root, archive_name(manifest.name, manifest.version, target), paths,
                          most_target_archive);
        if (!archive) {
            return std::unexpected(archive.error());
        }

        if (const std::optional<std::string> wrong =
                target_size_error(archive->file, archive->ref.size, on_disk)) {
            return std::unexpected(*wrong);
        }

        archive->target = target;
        package.entry.targets.emplace(target, archive->ref);
        package.targets.push_back(std::move(*archive));
    }

    return package;
}

std::expected<std::vector<std::string>, std::string>
publish_package(const prepared_package_t& package, const std::string_view docs,
                const registry_session_t& session) {
    const std::string& name = package.manifest.name;
    const std::string& version = package.manifest.version;
    if (docs.size() > most_docs_bytes) {
        return std::unexpected("fb doc's JSON is more than the registry's 25 MB");
    }

    registry_t registry(session.index, session.http, false, session.status);
    const std::expected<std::vector<index_entry_t>, std::string> published =
        registry.versions(name);
    if (!published) {
        return std::unexpected(published.error());
    }

    if (std::ranges::contains(*published, version, &index_entry_t::version)) {
        return std::unexpected(std::format(
            "{} v{} is published already, and a version can't be replaced; publishing needs a new "
            "version in project.toml",
            name, version));
    }

    if (!registry.notes().empty()) {
        return std::unexpected(registry.notes().front());
    }

    if (registry.local()) {
        if (const std::expected<void, std::string> written = publish_locally(package, session);
            !written) {
            return std::unexpected(written.error());
        }

        return std::vector<std::string>{};
    }

    if (session.token.empty()) {
        return std::unexpected("publishing needs a token: fb login, or FB_REGISTRY_TOKEN");
    }

    json_t archives = json_t::object();
    for (const built_archive_t* archive : { &package.source }) {
        if (const std::expected<void, std::string> sent =
                upload(session, archive->file, archive->bytes, archive->ref);
            !sent) {
            return std::unexpected(sent.error());
        }

        archives["source"] = { { "sha256", archive->ref.sha256 }, { "size", archive->ref.size } };
    }

    for (const built_archive_t& archive : package.targets) {
        if (const std::expected<void, std::string> sent =
                upload(session, archive.file, archive.bytes, archive.ref);
            !sent) {
            return std::unexpected(sent.error());
        }

        archives[archive.target] = {
            { "sha256", archive.ref.sha256 },
            { "size", archive.ref.size },
        };
    }

    json_t docs_ref = nullptr;
    if (!docs.empty()) {
        const archive_ref_t ref{ .sha256 = sha256_hex(docs), .size = docs.size() };
        if (const std::expected<void, std::string> sent =
                upload(session, std::format("{}'s docs", name), docs, ref);
            !sent) {
            return std::unexpected(sent.error());
        }

        docs_ref = { { "sha256", ref.sha256 }, { "size", ref.size } };
    }

    const std::expected<http_response_t, std::string> done = call(
        session, "POST", std::format("/packages/{}/versions", name),
        { { "version", version }, { "archives", archives }, { "docs", docs_ref } }, { 201, 200 });
    if (!done) {
        return std::unexpected(done.error());
    }

    std::vector<std::string> warnings;
    const json_t answer = json_t::parse(done->body, nullptr, false);
    if (answer.is_object() && answer.contains("warnings") && answer["warnings"].is_array()) {
        for (const json_t& warning : answer["warnings"]) {
            if (warning.is_string()) {
                warnings.push_back(warning.get<std::string>());
            }
        }
    }

    return warnings;
}

std::expected<void, std::string> yank_version(const std::string_view name,
                                              const std::string_view version, const bool undo,
                                              const registry_session_t& session) {
    if (!local_index_folder(session.index).empty()) {
        return yank_locally(name, version, !undo, session);
    }

    const std::expected<http_response_t, std::string> done =
        call(session, "POST",
             std::format("/packages/{}/versions/{}/{}", name, version, undo ? "unyank" : "yank"),
             nullptr, { 200 });
    if (!done) {
        return std::unexpected(done.error());
    }

    return {};
}

std::expected<std::vector<package_owner_t>, std::string>
list_owners(const std::string_view name, const registry_session_t& session) {
    if (!local_index_folder(session.index).empty()) {
        return std::unexpected("a registry on disk has no owners");
    }

    const std::expected<http_response_t, std::string> done =
        call(session, "GET", std::format("/packages/{}/owners", name), nullptr, { 200 });
    if (!done) {
        return std::unexpected(done.error());
    }

    std::vector<package_owner_t> owners;
    const json_t answer = json_t::parse(done->body, nullptr, false);
    if (answer.is_object() && answer.contains("owners") && answer["owners"].is_array()) {
        for (const json_t& owner : answer["owners"]) {
            if (owner.is_object()) {
                owners.push_back(
                    { .kind = text_field(owner, "kind"), .name = text_field(owner, "name") });
            }
        }
    }

    return owners;
}

std::expected<std::string, std::string> change_owner(const std::string_view name,
                                                     const std::string_view who, const bool remove,
                                                     const registry_session_t& session) {
    if (!local_index_folder(session.index).empty()) {
        return std::unexpected("a registry on disk has no owners");
    }

    const bool team = who.starts_with("team:");
    const std::string named(team ? who.substr(5) : who);
    const json_t body = { { team ? "team" : "user", named } };
    const std::expected<http_response_t, std::string> done = call(
        session, remove ? "DELETE" : "POST", std::format("/packages/{}/owners", name), body,
        remove ? std::initializer_list<long>{ 204, 200 } : std::initializer_list<long>{ 201, 202 });
    if (!done) {
        return std::unexpected(done.error());
    }

    if (remove) {
        return std::format("{} {} no longer owns {}", team ? "team" : "user", named, name);
    }

    return done->status == 202
               ? std::format("{} is invited to own {}, which they accept on the package site",
                             named, name)
               : std::format("team {} owns {}", named, name);
}

std::expected<std::pair<std::vector<found_package_t>, std::size_t>, std::string>
search_packages(const std::string_view words, const std::size_t limit,
                const registry_session_t& session) {
    std::vector<found_package_t> found;
    if (const std::filesystem::path folder = local_index_folder(session.index); !folder.empty()) {
        // A registry on disk is searched by its packages' names, which are lowercase: a name
        // with any of the words in it.
        std::vector<std::string> wanted;
        for (const auto word : std::views::split(words, ' ')) {
            std::string lower(word.begin(), word.end());
            std::ranges::transform(lower, lower.begin(), [](const char c) {
                return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
            });
            if (!lower.empty()) {
                wanted.push_back(std::move(lower));
            }
        }

        std::error_code error;
        std::vector<std::string> names;
        for (std::filesystem::directory_iterator each(folder, error), end; !error && each != end;
             each.increment(error)) {
            const std::string name = to_utf8(each->path().filename());
            const bool matches =
                wanted.empty() || std::ranges::any_of(wanted, [&](const std::string& part) {
                    return name.contains(part);
                });
            if (each->is_regular_file(error) && !name.contains('.') && matches) {
                names.push_back(name);
            }
        }

        std::ranges::sort(names);
        for (const std::string& name : names) {
            const std::optional<std::string> text = read_bytes(folder / path_from_utf8(name));
            const std::expected<std::vector<index_entry_t>, std::string> entries =
                text ? parse_index(*text, name) : std::vector<index_entry_t>{};
            std::optional<version_t> newest;
            for (const index_entry_t& entry : entries.value_or(std::vector<index_entry_t>{})) {
                const std::expected<version_t, std::string> version = parse_version(entry.version);
                if (version && !entry.yanked && (!newest || *version > *newest)) {
                    newest = *version;
                }
            }

            if (newest) {
                found.push_back({ .name = name, .version = newest->text() });
            }
        }

        const std::size_t total = found.size();
        if (found.size() > limit) {
            found.resize(limit);
        }

        return std::pair(std::move(found), total);
    }

    const std::expected<http_response_t, std::string> done =
        call(session, "GET", std::format("/search?q={}&per_page={}", url_escape(words), limit),
             nullptr, { 200 });
    if (!done) {
        return std::unexpected(done.error());
    }

    const json_t answer = json_t::parse(done->body, nullptr, false);
    std::size_t total = 0;
    if (answer.is_object() && answer.contains("packages") && answer["packages"].is_array()) {
        for (const json_t& each : answer["packages"]) {
            if (each.is_object()) {
                found.push_back({
                    .name = text_field(each, "name"),
                    .version = text_field(each, "max_version"),
                    .description = text_field(each, "description"),
                });
            }
        }

        total = answer.contains("total") && answer["total"].is_number_unsigned()
                    ? answer["total"].get<std::size_t>()
                    : found.size();
    }

    return std::pair(std::move(found), total);
}

std::expected<std::string, std::string>
login(const std::string_view name, const registry_session_t& session, const login_hooks_t& hooks) {
    if (!local_index_folder(session.index).empty()) {
        return std::unexpected("a registry on disk has no accounts to sign in to");
    }

    const std::expected<http_response_t, std::string> started =
        call(session, "POST", "/cli-logins", { { "name", name } }, { 201 });
    if (!started) {
        return std::unexpected(
            std::format("the registry couldn't start a sign-in: {}", started.error()));
    }

    const json_t answer = json_t::parse(started->body, nullptr, false);
    const std::string id = answer.is_object() ? text_field(answer, "id") : "";
    const std::string secret = answer.is_object() ? text_field(answer, "secret") : "";
    const std::string url = answer.is_object() ? text_field(answer, "url") : "";
    const std::string code = answer.is_object() ? text_field(answer, "code") : "";
    if (id.empty() || secret.empty() || url.empty()) {
        return std::unexpected("the registry's answer to a sign-in had no id, secret, and url");
    }

    // Only a page over TLS, or on this machine, is opened; the code is shown first, so it's
    // there however long the browser takes.
    const bool openable = (url.starts_with("https://") || is_loopback_url(url)) && hooks.open;
    if (hooks.show) {
        hooks.show(code, url, openable);
    }

    if (openable) {
        (void)hooks.open(url);
    }

    std::int64_t interval = 3;
    if (const auto given = answer.find("interval");
        given != answer.end() && given->is_number_integer()) {
        interval = std::clamp<std::int64_t>(given->get<std::int64_t>(), 1, 60);
    }

    const std::expected<std::string, std::string> api = api_of(session);
    if (!api) {
        return std::unexpected(api.error());
    }

    // The registry's sign-ins last 10 minutes; this stops a little after. A poll that's rate
    // limited, or that the network or the registry fails, is tried again.
    constexpr std::int64_t most_seconds = 660;
    std::int64_t wait = interval;
    for (std::int64_t waited = 0; waited < most_seconds; waited += wait) {
        if (hooks.wait) {
            hooks.wait(std::chrono::seconds(wait));
        }

        wait = interval;
        const std::expected<http_response_t, std::string> polled = session.http({
            .method = "POST",
            .url = std::format("{}/cli-logins/{}/poll", *api, id),
            .headers = { { "Accept", "application/json" }, { "Content-Type", "application/json" } },
            .body = json_t({ { "secret", secret } }).dump(),
        });
        if (!polled || polled->status == 202 || polled->status >= 500) {
            continue;
        }

        if (polled->status == 429) {
            // Retry-After's seconds, within reason.
            const auto after = polled->headers.find("retry-after");
            std::int64_t seconds = 0;
            if (after != polled->headers.end()) {
                (void)std::from_chars(after->second.data(),
                                      after->second.data() + after->second.size(), seconds);
            }

            wait = std::clamp<std::int64_t>(seconds, interval, 120);
            continue;
        }

        if (polled->status == 410) {
            return std::unexpected(
                "the sign-in was denied, or it expired; fb login starts another");
        }

        if (polled->status != 200) {
            return std::unexpected(
                std::format("the registry refused the sign-in: {}", api_error(*polled)));
        }

        const json_t approved = json_t::parse(polled->body, nullptr, false);
        std::string token = approved.is_object() ? text_field(approved, "token") : "";
        if (token.empty()) {
            return std::unexpected("the registry approved the sign-in, but sent no token");
        }

        return token;
    }

    return std::unexpected("the sign-in wasn't approved in time; fb login starts another");
}

std::optional<std::expected<std::string, std::string>>
trusted_publishing_token(const registry_session_t& session) {
    const std::optional<std::string> request_url =
        environment_variable("ACTIONS_ID_TOKEN_REQUEST_URL");
    const std::optional<std::string> request_token =
        environment_variable("ACTIONS_ID_TOKEN_REQUEST_TOKEN");
    if (!request_url || !request_token) {
        return std::nullopt;
    }

    registry_t registry(session.index, session.http, false, session.status);
    const std::expected<registry_config_t, std::string> config = registry.config();
    if (!config) {
        return std::unexpected(config.error());
    }

    if (config->oidc_audience.empty()) {
        return std::unexpected(std::format(
            "the registry at {} doesn't take trusted publishing: its config.json has no "
            "oidc_audience",
            session.index));
    }

    const char separator = request_url->contains('?') ? '&' : '?';
    const std::expected<http_response_t, std::string> github = session.http({
        .url = std::format("{}{}audience={}", *request_url, separator,
                           url_escape(config->oidc_audience)),
        .headers = { { "Authorization", std::format("Bearer {}", *request_token) } },
    });
    if (!github) {
        return std::unexpected(github.error());
    }

    const json_t answer = json_t::parse(github->body, nullptr, false);
    const std::string jwt = answer.is_object() ? text_field(answer, "value") : "";
    if (github->status != 200 || jwt.empty()) {
        return std::unexpected(std::format(
            "GitHub gave no OIDC token, which needs `permissions: id-token: write` in the "
            "workflow: it answered {}",
            github->status));
    }

    const std::expected<http_response_t, std::string> exchanged =
        call(session, "POST", "/trusted-publishing/token", { { "token", jwt } }, { 201, 200 });
    if (!exchanged) {
        return std::unexpected(
            std::format("the registry didn't take the workflow's token: {}", exchanged.error()));
    }

    const json_t token = json_t::parse(exchanged->body, nullptr, false);
    std::string value = token.is_object() ? text_field(token, "token") : "";
    if (value.empty()) {
        return std::unexpected("the registry's answer to trusted publishing had no token");
    }

    return value;
}

std::expected<void, std::string> revoke_token(const registry_session_t& session) {
    const std::expected<http_response_t, std::string> done =
        call(session, "DELETE", "/tokens/current", nullptr, { 204, 200 });
    if (!done) {
        return std::unexpected(done.error());
    }

    return {};
}

std::optional<std::string> environment_variable(const std::string_view name) {
#ifdef _WIN32
    const std::wstring wide = path_from_utf8(name).wstring();
    const wchar_t* value = _wgetenv(wide.c_str());
    if (value == nullptr) {
        return std::nullopt;
    }

    // As the value is, not as a path: to_utf8() would turn its '\'s into '/'s and run "//"s
    // together, as in a URL.
    const std::u8string text = std::filesystem::path(value).u8string();
    return std::string(text.begin(), text.end());
#else
    const char* value = std::getenv(std::string(name).c_str());
    if (value == nullptr) {
        return std::nullopt;
    }

    return std::string(value);
#endif
}

} // namespace fb
