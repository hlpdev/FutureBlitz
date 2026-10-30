#include "fb/package_archive.hpp"
#include "fb/registry.hpp"
#include "package_helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace {

/// Hashes of no archive, for index lines.
std::string hash_a() {
    std::string hash(64, 'a');
    return hash;
}

std::string hash_b() {
    std::string hash(64, 'b');
    return hash;
}

/// A registry on the network that's a map of URLs to answers, which records what it was asked.
struct fake_server_t {
    std::map<std::string, fb::http_response_t, std::less<>> answers;
    std::vector<fb::http_request_t> requests;
    bool down = false;

    [[nodiscard]] fb::http_client_t client() {
        return [this](const fb::http_request_t& request)
                   -> std::expected<fb::http_response_t, std::string> {
            requests.push_back(request);
            if (down) {
                return std::unexpected(
                    std::format("{} couldn't be reached: no route", request.url));
            }

            const auto found = answers.find(request.url);
            if (found == answers.end()) {
                return fb::http_response_t{ .status = 404 };
            }

            return found->second;
        };
    }
};

std::string header(const fb::http_request_t& request, const std::string_view name) {
    for (const auto& [field, value] : request.headers) {
        if (field == name) {
            return value;
        }
    }

    return "";
}

} // namespace

TEST_CASE("an index line is docs/registry-api.md's, and reads back the same", "[registry]") {
    const std::string line =
        R"({"v":1,"name":"physics","version":"1.2.0","deps":[{"name":"math3d","req":"0.4"}],)"
        R"("fb":"0.9","targets":["linux-x64","windows-x64"],"archives":{"source":{"sha256":")" +
        hash_a() + R"(","size":48211},"linux-x64":{"sha256":")" + hash_b() +
        R"(","size":5120334},"windows-x64":{"sha256":")" + hash_b() +
        R"(","size":5023117}},"yanked":false})";
    const std::expected<std::vector<fb::index_entry_t>, std::string> entries =
        fb::parse_index(line + "\n", "physics");
    REQUIRE(entries);
    REQUIRE(entries->size() == 1);
    const fb::index_entry_t& entry = entries->front();
    CHECK(entry.version == "1.2.0");
    REQUIRE(entry.dependencies.size() == 1);
    CHECK(entry.dependencies[0].name == "math3d");
    CHECK(entry.dependencies[0].req == "0.4");
    CHECK_FALSE(entry.dependencies[0].registry);
    CHECK(entry.fb == "0.9");
    CHECK(entry.source.sha256 == hash_a());
    CHECK(entry.source.size == 48211);
    CHECK(entry.targets.at("windows-x64").size == 5023117);
    CHECK_FALSE(entry.yanked);
    CHECK(fb::index_line(entry) == line);

    // A later format's line is a later fb's, and a key fb doesn't know is left alone.
    const std::string later = R"({"v":2,"name":"physics","version":"9.0.0"})";
    const std::string more = R"({"v":1,"name":"physics","version":"1.3.0","new_key":5,"deps":[],)"
                             R"("fb":null,"targets":[],"archives":{"source":{"sha256":")" +
                             hash_a() + R"(","size":1}},"yanked":true})";
    const std::expected<std::vector<fb::index_entry_t>, std::string> mixed =
        fb::parse_index(line + "\n" + later + "\r\n" + more + "\n\n", "physics");
    REQUIRE(mixed);
    REQUIRE(mixed->size() == 2);
    CHECK(mixed->at(1).yanked);
    CHECK_FALSE(mixed->at(1).fb);

    for (const std::string& wrong : {
             std::string("not json"),
             std::string(R"({"v":1,"name":"other","version":"1.0.0"})"),
             R"({"v":1,"name":"physics","version":"1.0","archives":{"source":{"sha256":")" +
                 hash_a() + R"(","size":1}}})",
             std::string(R"({"v":1,"name":"physics","version":"1.0.0","archives":{}})"),
             R"({"v":1,"name":"physics","version":"1.0.0","targets":["linux-x64"],)"
             R"("archives":{"source":{"sha256":")" +
                 hash_a() + R"(","size":1}}})",
         }) {
        INFO(wrong);
        const std::expected<std::vector<fb::index_entry_t>, std::string> read =
            fb::parse_index(wrong, "physics");
        REQUIRE_FALSE(read);
        CHECK(read.error() == "line 1 of physics's index file isn't a version of it");
    }
}

TEST_CASE("config.json says where archives are, as a template", "[registry]") {
    const std::expected<fb::registry_config_t, std::string> config = fb::parse_registry_config(
        R"({"v":1,"dl":"https://dl.futureblitz.dev/{name}/{version}/{file}",)"
        R"("api":"https://pkg.futureblitz.dev/api/v1","oidc_audience":"pkg.futureblitz.dev"})");
    REQUIRE(config);
    CHECK(config->api == "https://pkg.futureblitz.dev/api/v1");
    CHECK(config->oidc_audience == "pkg.futureblitz.dev");
    CHECK(fb::download_url(config->dl, "physics", "1.2.0", "physics-1.2.0.fbpkg") ==
          "https://dl.futureblitz.dev/physics/1.2.0/physics-1.2.0.fbpkg");
    CHECK(fb::download_url("https://dl.example.com/files", "a", "1.0.0", "a-1.0.0.fbpkg") ==
          "https://dl.example.com/files/a/1.0.0/a-1.0.0.fbpkg");
    CHECK_FALSE(fb::parse_registry_config(R"({"api":"x"})"));
    CHECK_FALSE(fb::parse_registry_config("[]"));
    CHECK(fb::archive_name("physics", "1.2.0") == "physics-1.2.0.fbpkg");
    CHECK(fb::archive_name("physics", "1.2.0", "linux-x64") == "physics-1.2.0-linux-x64.fbpkg");
}

TEST_CASE("an index is named by its URL, or for a folder, a file:// URL", "[registry]") {
    CHECK(fb::canonical_index("https://index.futureblitz.dev", "") ==
          "https://index.futureblitz.dev/");
    CHECK(fb::canonical_index("http://127.0.0.1:9/index/", "") == "http://127.0.0.1:9/index/");
    const fb::testing::sandbox_t sandbox;
    std::filesystem::create_directories(sandbox / "registry/index");
    const std::string canonical = fb::canonical_index("registry/index", sandbox.folder());
    CHECK(canonical.starts_with("file:///"));
    CHECK(canonical.ends_with("/registry/index/"));
    CHECK(fb::canonical_index(canonical, "elsewhere") == canonical);
    CHECK(
        std::filesystem::equivalent(fb::local_index_folder(canonical), sandbox / "registry/index"));
    CHECK(fb::local_index_folder("https://index.futureblitz.dev/").empty());

    const std::string folder = fb::registry_folder("https://index.futureblitz.dev/");
    CHECK(folder.starts_with("index.futureblitz.dev-"));
    CHECK(folder.size() == std::string_view("index.futureblitz.dev-").size() + 16);
    CHECK(fb::registry_folder("https://index.futureblitz.dev/") !=
          fb::registry_folder("https://index.futureblitz.dev/mirror/"));
    CHECK(fb::registry_folder(canonical).starts_with("index-"));
    CHECK(fb::package_cache_folder("https://index.futureblitz.dev/", "physics", "1.0.0") ==
          fb::fb_home() / "cache" / "packages" / folder / "physics-1.0.0");
}

TEST_CASE("a registry's index files are cached with their ETags", "[registry]") {
    const fb::testing::sandbox_t sandbox;
    const std::string line = R"({"v":1,"name":"physics","version":"1.0.0","deps":[],"fb":null,)"
                             R"("targets":[],"archives":{"source":{"sha256":")" +
                             hash_a() + R"(","size":1}},"yanked":false})";
    fake_server_t server;
    server.answers["https://index.example.com/physics"] = {
        .status = 200,
        .headers = { { "etag", "\"v1\"" } },
        .body = line + "\n",
    };
    {
        fb::registry_t registry("https://index.example.com/", server.client(), false, {});
        const auto versions = registry.versions("physics");
        REQUIRE(versions);
        CHECK(versions->size() == 1);
        CHECK(header(server.requests.back(), "If-None-Match").empty());
        CHECK(registry.versions("nope")->empty());
    }

    // The next time, it asks with the ETag, and an unchanged file is the cache's.
    server.answers["https://index.example.com/physics"] = { .status = 304 };
    {
        fb::registry_t registry("https://index.example.com/", server.client(), false, {});
        const auto versions = registry.versions("physics");
        REQUIRE(versions);
        CHECK(versions->size() == 1);
        CHECK(header(server.requests.back(), "If-None-Match") == "\"v1\"");
        CHECK(registry.notes().empty());
    }

    // Offline, or when the registry can't be reached, it's the cache's, which it says.
    server.requests.clear();
    {
        fb::registry_t registry("https://index.example.com/", server.client(), true, {});
        CHECK(registry.versions("physics")->size() == 1);
        CHECK(server.requests.empty());
        const auto missing = registry.versions("shapes");
        REQUIRE_FALSE(missing);
        CHECK(missing.error().contains("fb is offline, and the cache has no index of shapes"));
    }

    server.down = true;
    {
        fb::registry_t registry("https://index.example.com/", server.client(), false, {});
        CHECK(registry.versions("physics")->size() == 1);
        REQUIRE(registry.notes().size() == 1);
        CHECK(registry.notes()[0].contains("so physics's versions are the cache's"));
        // It isn't asked again once it couldn't be reached.
        const std::size_t asked = server.requests.size();
        CHECK_FALSE(registry.versions("shapes"));
        CHECK(server.requests.size() == asked);
    }

    server.down = false;
    server.answers["https://index.example.com/physics"] = {
        .status = 500,
        .body = R"({"errors":[{"code":"internal","detail":"the index is down"}]})",
    };
    fb::registry_t registry("https://index.example.com/", server.client(), false, {});
    const auto failed = registry.versions("physics");
    REQUIRE_FALSE(failed);
    CHECK(failed.error().contains("the index is down"));
}

TEST_CASE("an archive is downloaded once, and checked against its hash", "[registry]") {
    const fb::testing::sandbox_t sandbox;
    const std::string bytes = "archive bytes";
    const std::string hash = fb::sha256_hex(bytes);
    fake_server_t server;
    server.answers["https://index.example.com/config.json"] = {
        .status = 200,
        .body = R"({"v":1,"dl":"https://dl.example.com/{name}/{version}/{file}","api":"x"})",
    };
    server.answers["https://dl.example.com/physics/1.0.0/physics-1.0.0.fbpkg"] = {
        .status = 200,
        .body = bytes,
    };
    std::vector<std::string> said;
    const fb::status_t status = [&](const std::string_view verb, const std::string_view message) {
        said.push_back(std::format("{} {}", verb, message));
    };
    // What an archive's file holds, or its error.
    const auto held = [](const std::expected<std::filesystem::path, std::string>& file) {
        if (!file) {
            return "error: " + file.error();
        }

        std::ifstream in(*file, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };
    {
        fb::registry_t registry("https://index.example.com/", server.client(), false, status);
        const auto downloaded =
            registry.archive("physics", "1.0.0", "physics-1.0.0.fbpkg", hash, 1000);
        CHECK(held(downloaded) == bytes);
        CHECK(downloaded.value_or("").parent_path().parent_path().filename() == "archives");
        CHECK(said == std::vector<std::string>{ "Downloading physics v1.0.0" });
        // Into a file of the request's, as it came, which is the cache's once it's checked.
        CHECK_FALSE(server.requests.back().save_to.empty());

        // m10-journey-16: a target's archive says its target.
        server.answers["https://dl.example.com/physics/1.0.0/physics-1.0.0-linux-x64.fbpkg"] = {
            .status = 200,
            .body = bytes,
        };
        said.clear();
        CHECK(held(registry.archive("physics", "1.0.0", "physics-1.0.0-linux-x64.fbpkg", hash,
                                    1000)) == bytes);
        CHECK(said == std::vector<std::string>{ "Downloading physics v1.0.0 (linux-x64)" });
    }

    // From the cache, even offline.
    server.requests.clear();
    {
        fb::registry_t registry("https://index.example.com/", server.client(), true, status);
        CHECK(held(registry.archive("physics", "1.0.0", "physics-1.0.0.fbpkg", hash, 1000)) ==
              bytes);
        CHECK(server.requests.empty());
    }

    // An archive that isn't what fb.lock says isn't used.
    server.answers["https://dl.example.com/physics/1.0.0/physics-1.0.0.fbpkg"].body = "changed";
    fb::registry_t registry("https://index.example.com/", server.client(), false, status);
    const auto wrong = registry.archive("physics", "1.0.0", "physics-1.0.0.fbpkg", hash_b(), 1000);
    REQUIRE_FALSE(wrong);
    CHECK(wrong.error() == std::format("physics-1.0.0.fbpkg isn't what fb.lock says it is: its "
                                       "SHA-256 is {}, and fb.lock's is {}",
                                       fb::sha256_hex("changed"), hash_b()));
    const auto missing = registry.archive("physics", "2.0.0", "physics-2.0.0.fbpkg", hash, 1000);
    REQUIRE_FALSE(missing);
    CHECK(missing.error().contains("couldn't be downloaded: it answered 404"));
}
