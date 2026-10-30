#include "fb/credentials.hpp"
#include "fb/driver.hpp"
#include "fb/http_client.hpp"
#include "fb/lower.hpp"
#include "fb/packages.hpp"
#include "fb/process.hpp"
#include "fb/registry_api.hpp"
#include "package_helpers.hpp"
#include "registry_server.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

/// A package on disk ready to publish to the test registry, whose dependencies are its.
void library(const fb::testing::sandbox_t& sandbox, const fb::testing::registry_server_t& server,
             const std::string_view name, const std::string_view version,
             const std::string_view dependencies, const std::string_view code) {
    sandbox.library(name, name, version, dependencies, { { "code.fb", std::string(code) } });
    // Its registry is the server, by the name the dependencies give it.
    std::string toml = sandbox.read(std::string(name) + "/project.toml");
    toml.replace(toml.find("../registry/index"), std::string_view("../registry/index").size(),
                 server.index());
    sandbox.write(std::string(name) + "/project.toml", toml);
}

fb::registry_session_t session_of(const fb::testing::registry_server_t& server,
                                  const std::string_view token = fb::testing::test_token) {
    return { .index = server.index(), .http = fb::send_http, .token = std::string(token) };
}

/// Builds and publishes the package in `folder` to the test registry.
std::expected<std::vector<std::string>, std::string> publish(const fb::testing::sandbox_t& sandbox,
                                                             const fb::registry_session_t& session,
                                                             const std::string_view folder) {
    const std::expected<fb::prepared_package_t, std::string> prepared =
        fb::prepare_package(sandbox / folder, session.index, false);
    if (!prepared) {
        return std::unexpected(prepared.error());
    }

    return fb::publish_package(*prepared, R"({"format":1})", session);
}

/// What a store keeps under `key`: its token, "none", or "error: ..." when it can't say.
std::string kept(fb::credential_store_t& store, const std::string_view key) {
    const std::expected<std::optional<std::string>, std::string> token = store.get(key);
    if (!token) {
        return "error: " + token.error();
    }

    return token->value_or("none");
}

std::size_t count_of(const std::vector<std::string>& requests, const std::string_view start) {
    return static_cast<std::size_t>(std::ranges::count_if(
        requests, [&](const std::string& request) { return request.starts_with(start); }));
}

} // namespace

TEST_CASE("fb's HTTP sends requests and reads answers, never leaving this machine", "[http]") {
    const fb::testing::http_server_t server([](const fb::http_request_t& request) {
        std::string seen = std::format("{} {} {}", request.method, request.url, request.body);
        for (const auto& [name, value] : request.headers) {
            if (name == "x-test" || name == "authorization") {
                seen += std::format(" {}={}", name, value);
            }
        }

        return fb::http_response_t{
            .status = request.url == "/missing" ? 404 : 200,
            .headers = { { "ETag", "\"abc\"" } },
            .body = seen,
        };
    });
    const std::expected<fb::http_response_t, std::string> got =
        fb::send_http({ .url = server.url() + "thing?q=1", .headers = { { "X-Test", "yes" } } });
    REQUIRE(got);
    CHECK(got->status == 200);
    CHECK(got->body == "GET /thing?q=1  x-test=yes");
    CHECK(got->headers.at("etag") == "\"abc\"");

    const std::expected<fb::http_response_t, std::string> put = fb::send_http({
        .method = "PUT",
        .url = server.url() + "up",
        .headers = { { "Authorization", "Bearer t" } },
        .body = std::string("bytes\0with nul", 14),
    });
    REQUIRE(put);
    CHECK(put->body == std::string("PUT /up bytes\0with nul authorization=Bearer t", 45));
    CHECK(fb::send_http({ .method = "HEAD", .url = server.url() + "x" })->body.empty());
    CHECK(fb::send_http({ .url = server.url() + "missing" })->status == 404);
    CHECK(fb::send_http({ .method = "DELETE", .url = server.url() + "gone" })->body ==
          "DELETE /gone ");

    // Nothing listens on a port the server let go of.
    std::string refused;
    {
        const fb::testing::http_server_t closed(
            [](const fb::http_request_t&) { return fb::http_response_t{}; });
        refused = closed.url();
    }

    const std::expected<fb::http_response_t, std::string> nothing =
        fb::send_http({ .url = refused });
    REQUIRE_FALSE(nothing);
    CHECK(nothing.error().starts_with(refused + " couldn't be reached: "));
    CHECK(fb::url_escape("a b&c/é") == "a%20b%26c%2F%C3%A9");
}

TEST_CASE("a package published over HTTP is added and built on another computer",
          "[registry-api]") {
    const fb::testing::registry_server_t server;
    const fb::registry_session_t session = session_of(server);
    {
        // One computer publishes shapes and physics.
        const fb::testing::sandbox_t publisher;
        library(publisher, server, "shapes", "0.3.0", "",
                "Public Function Version(): String\n    Return \"shapes 0.3.0\"\nEnd Function\n");
        REQUIRE(publish(publisher, session, "shapes"));
        library(publisher, server, "physics", "1.2.0",
                "shapes = { version = \"0.3\", registry = \"local\" }\n",
                "Import shapes\nPublic Function Describe(): String\n"
                "    Return \"physics with \" + Version()\nEnd Function\n");
        const std::expected<std::vector<std::string>, std::string> warnings =
            publish(publisher, session, "physics");
        INFO((warnings ? std::string() : warnings.error()));
        REQUIRE(warnings);
        CHECK(*warnings == std::vector<std::string>{ "the readme has no title" });

        // A version is published once.
        const std::expected<std::vector<std::string>, std::string> again =
            publish(publisher, session, "physics");
        REQUIRE_FALSE(again);
        CHECK(again.error().contains("physics v1.2.0 is published already"));
    }

    // Each archive and the docs went up once, by their SHA-256s, before each version: the docs,
    // the same for both, are uploaded once.
    const std::vector<std::string> requests = server.requests();
    CHECK(count_of(requests, "PUT /api/v1/uploads/") == 3);
    CHECK(count_of(requests, "HEAD /api/v1/uploads/") == 4);
    CHECK(count_of(requests, "POST /api/v1/packages/physics/versions") == 1);
    // commands-13: the API's URL comes from config.json once a session.
    CHECK(count_of(requests, "GET /index/config.json") == 1);

    // Another adds physics, and builds with it.
    const fb::testing::sandbox_t other;
    other.write("game/project.toml",
                std::format("[project]\nname = \"game\"\nversion = \"0.1.0\"\n\n[registries]\n"
                            "local = {{ index = \"{}\" }}\n\n[dependencies]\nphysics = {{ version "
                            "= \"1.2.0\", registry = \"local\" }}\n",
                            server.index()));
    other.write("game/main.fb", "Import std.core\nImport physics\nPrint(Describe())\n");
    std::vector<std::string> said;
    const fb::sync_options_t options{
        .status =
            [&](const std::string_view verb, const std::string_view message) {
                said.push_back(std::format("{} {}", verb, message));
            },
    };
    const std::expected<fb::sync_result_t, std::string> synced =
        fb::sync_packages(other / "game", options);
    INFO((synced ? std::string() : synced.error()));
    REQUIRE(synced);
    CHECK(said == std::vector<std::string>{
                      "Adding physics v1.2.0",
                      "Adding shapes v0.3.0",
                      "Downloading physics v1.2.0",
                      "Downloading shapes v0.3.0",
                  });

    const std::expected<fb::compilation_t, std::string> game = fb::compile_file(other / "game");
    REQUIRE(game);
    REQUIRE(game->diagnostics.error_count() == 0);
    std::filesystem::path program = other / "game/build/game";
    program += fb::executable_extension;
    const std::expected<fb::process_result_t, std::string> ran =
        fb::build_program(fb::lower(game->ast, game->analysis), *game, program,
                          fb::build_mode_t::DEBUG)
            .and_then([&] { return fb::run_process(program, {}); });
    INFO((ran ? ran->output : ran.error()));
    REQUIRE(ran);
    CHECK(ran->output == "physics with shapes 0.3.0\n");

    // Offline, from the cache, with its index files: fb update keeps what's there.
    said.clear();
    std::filesystem::remove(other / "game/fb.lock");
    const std::expected<fb::sync_result_t, std::string> offline =
        fb::sync_packages(other / "game", { .offline = true, .status = options.status });
    REQUIRE(offline);
    CHECK(said == std::vector<std::string>{ "Adding physics v1.2.0", "Adding shapes v0.3.0" });

    // Online again, the unchanged index files are 304s for their ETags.
    std::filesystem::remove(other / "game/fb.lock");
    REQUIRE(fb::sync_packages(other / "game", options));
    CHECK(server.requests().back().starts_with("GET /index/"));
}

TEST_CASE("publishing needs the registry's token", "[registry-api]") {
    const fb::testing::registry_server_t server;
    const fb::testing::sandbox_t sandbox;
    library(sandbox, server, "shapes", "0.3.0", "", "// shapes\n");
    const std::expected<std::vector<std::string>, std::string> none =
        publish(sandbox, session_of(server, ""), "shapes");
    REQUIRE_FALSE(none);
    CHECK(none.error() == "publishing needs a token: fb login, or FB_REGISTRY_TOKEN");
    const std::expected<std::vector<std::string>, std::string> wrong =
        publish(sandbox, session_of(server, "fbr_nope"), "shapes");
    REQUIRE_FALSE(wrong);
    CHECK(wrong.error().contains("a token is needed, and that one isn't it"));
}

TEST_CASE("a large archive is uploaded in parts", "[registry-api]") {
    fb::testing::registry_server_t server;
    server.part_size = 700;
    const fb::testing::sandbox_t sandbox;
    std::string big;
    for (int i = 0; i < 400; ++i) {
        big += std::format("// line {} of a long comment, {}\n", i, i * 7919 % 1000);
    }

    library(sandbox, server, "shapes", "0.3.0", "", big);
    fb::registry_session_t session = session_of(server);
    session.most_single_upload = 1000;
    const std::expected<std::vector<std::string>, std::string> published =
        publish(sandbox, session, "shapes");
    INFO((published ? std::string() : published.error()));
    REQUIRE(published);
    const std::vector<std::string> requests = server.requests();
    CHECK(count_of(requests, "POST /api/v1/uploads/") == 2);
    CHECK(count_of(requests, "PUT /api/v1/uploads/") > 2);
    CHECK(count_of(requests, "POST /api/v1/packages/shapes/versions") == 1);
}

TEST_CASE("yanking, owners, and search go through the registry's API", "[registry-api]") {
    const fb::testing::registry_server_t server;
    const fb::registry_session_t session = session_of(server);
    const fb::testing::sandbox_t sandbox;
    for (const std::string_view version : { "1.0.0", "1.1.0" }) {
        library(sandbox, server, "physics", version, "", "// physics\n");
        REQUIRE(publish(sandbox, session, "physics"));
    }

    REQUIRE(fb::yank_version("physics", "1.1.0", false, session));
    fb::registry_t registry(server.index(), fb::send_http, false, {});
    const auto versions = registry.versions("physics");
    REQUIRE(versions);
    CHECK(versions->at(1).yanked);
    CHECK(fb::yank_version("physics", "9.0.0", false, session).error() == "no such version");
    REQUIRE(fb::yank_version("physics", "1.1.0", true, session));

    const auto owners = fb::list_owners("physics", session);
    REQUIRE(owners);
    REQUIRE(owners->size() == 1);
    CHECK(owners->front().name == "tester");
    CHECK(fb::change_owner("physics", "alice", false, session) ==
          "alice is invited to own physics, which they accept on the package site");
    CHECK(fb::change_owner("physics", "tester", true, session).error() ==
          "a package keeps one owner at least");

    const auto found = fb::search_packages("phys", 10, session);
    REQUIRE(found);
    REQUIRE(found->first.size() == 1);
    CHECK(found->first[0].name == "physics");
    CHECK(found->first[0].version == "1.1.0");
    CHECK(found->first[0].description == "A test package");
    CHECK(found->second == 1);
}

TEST_CASE("fb login opens the sign-in page and polls until it's approved", "[registry-api]") {
    fb::testing::registry_server_t server;
    server.pending_polls = 2;
    std::vector<std::string> opened;
    std::vector<std::chrono::seconds> waited;
    std::string shown;
    const fb::login_hooks_t hooks{
        .open =
            [&](const std::string_view url) {
                opened.emplace_back(url);
                return true;
            },
        .wait = [&](const std::chrono::seconds seconds) { waited.push_back(seconds); },
        .show = [&](const std::string_view code, const std::string_view,
                    const bool) { shown = code; },
    };
    const std::expected<std::string, std::string> token =
        fb::login("fb on test", session_of(server, ""), hooks);
    REQUIRE(token);
    CHECK(*token == fb::testing::test_token);
    CHECK(opened == std::vector<std::string>{ server.index().substr(0, server.index().size() - 6) +
                                              "cli-login/login1" });
    CHECK(shown == "WDJB-MJHT");
    CHECK(waited.size() == 3);
    CHECK(waited.front() == std::chrono::seconds(1));

    // A registry on disk has no accounts.
    const fb::testing::sandbox_t sandbox;
    CHECK(fb::login("fb", { .index = sandbox.registry() }, hooks).error().contains("on disk"));
}

TEST_CASE("fb login shows its code before it opens the page, and opens only a safe one",
          "[registry-api]") {
    // commands-1 and commands-10: the code's on screen before the browser starts, and a page
    // that isn't https:// or on this machine isn't opened.
    const fb::testing::registry_server_t server;
    std::vector<std::string> events;
    const fb::login_hooks_t hooks{
        .open =
            [&](const std::string_view url) {
                events.push_back(std::format("open {}", url));
                return true;
            },
        .wait = [](std::chrono::seconds) {},
        .show =
            [&](const std::string_view code, const std::string_view, const bool opening) {
                events.push_back(std::format("show {} {}", code, opening));
            },
    };
    REQUIRE(fb::login("fb on test", session_of(server, ""), hooks));
    REQUIRE(events.size() == 2);
    CHECK(events[0] == "show WDJB-MJHT true");
    CHECK(events[1].starts_with("open http://127.0.0.1:"));

    // A page elsewhere over plain http is shown, to open by hand, but not opened.
    fb::registry_session_t session = session_of(server, "");
    const fb::http_client_t elsewhere = [](const fb::http_request_t& request) {
        std::expected<fb::http_response_t, std::string> response = fb::send_http(request);
        if (response && request.url.ends_with("/cli-logins")) {
            std::string& body = response->body;
            const std::size_t at = body.find("http://127.0.0.1:");
            body.replace(at, body.find('/', at + 7) - at, "http://example.com");
        }

        return response;
    };
    session.http = elsewhere;
    events.clear();
    REQUIRE(fb::login("fb on test", session, hooks));
    CHECK(events == std::vector<std::string>{ "show WDJB-MJHT false" });
}

TEST_CASE("fb login polls through rate limits and failures, until it's denied or too late",
          "[registry-api]") {
    // commands-4: 429s wait as Retry-After says, and 5xx's and the network failing are tried
    // again.
    fb::testing::registry_server_t server;
    server.poll_statuses = { 429, 500, 503 };
    std::vector<std::chrono::seconds> waited;
    const fb::login_hooks_t hooks{
        .wait = [&](const std::chrono::seconds seconds) { waited.push_back(seconds); },
    };
    fb::registry_session_t session = session_of(server, "");
    int failures = 1;
    session.http =
        [&](const fb::http_request_t& request) -> std::expected<fb::http_response_t, std::string> {
        if (request.url.ends_with("/poll") && failures-- > 0) {
            return std::unexpected("the network is down");
        }

        return fb::send_http(request);
    };
    const std::expected<std::string, std::string> token = fb::login("fb", session, hooks);
    INFO((token ? std::string() : token.error()));
    REQUIRE(token);
    CHECK(*token == fb::testing::test_token);
    // The network's failure, the 429, the 500, the 503, the pending poll, then the approval.
    CHECK(waited == std::vector<std::chrono::seconds>{
                        std::chrono::seconds(1),
                        std::chrono::seconds(1),
                        std::chrono::seconds(7),
                        std::chrono::seconds(1),
                        std::chrono::seconds(1),
                        std::chrono::seconds(1),
                    });

    // Denied, or expired, it says so.
    server.poll_statuses = { 410 };
    const std::expected<std::string, std::string> denied =
        fb::login("fb", session_of(server, ""), hooks);
    REQUIRE_FALSE(denied);
    CHECK(denied.error() == "the sign-in was denied, or it expired; fb login starts another");

    // Never approved, it stops once the sign-in would have expired.
    server.poll_statuses.clear();
    server.pending_polls = 100'000;
    waited.clear();
    const std::expected<std::string, std::string> late =
        fb::login("fb", session_of(server, ""), hooks);
    REQUIRE_FALSE(late);
    CHECK(late.error() == "the sign-in wasn't approved in time; fb login starts another");
    CHECK(waited.size() == 660);

    // Another answer is the registry's refusal.
    server.poll_statuses = { 403 };
    CHECK(fb::login("fb", session_of(server, ""), hooks).error() ==
          "the registry refused the sign-in: try again");
}

TEST_CASE("a token goes only to an API over TLS, or on this machine", "[registry-api]") {
    // commands-2: config.json's api and dl are https://, or http:// on this machine.
    fb::testing::registry_server_t server;
    server.api_url = "http://example.com/api/v1";
    const std::expected<void, std::string> yanked =
        fb::yank_version("physics", "1.0.0", false, session_of(server));
    REQUIRE_FALSE(yanked);
    CHECK(yanked.error().contains("names http://example.com/api/v1 as its API, which isn't "
                                  "https://, so fb sends it no token"));
    CHECK(fb::login("fb", session_of(server, ""), {}).error().contains("which isn't https://"));
    for (const std::string& request : server.requests()) {
        CHECK(request == "GET /index/config.json");
    }

    fb::testing::registry_server_t downloads;
    downloads.dl_url = "http://example.com/dl/{name}/{version}/{file}";
    fb::registry_t registry(downloads.index(), fb::send_http, false, {});
    const std::expected<fb::registry_config_t, std::string> config = registry.config();
    REQUIRE_FALSE(config);
    CHECK(config.error().contains("http://example.com/dl/"));
}

TEST_CASE("fb publish checks what the registry checks, as the registry counts it",
          "[registry-api]") {
    const fb::testing::sandbox_t sandbox;
    const std::string index = sandbox.registry();
    const auto prepared_error = [&](const std::string_view folder) {
        const std::expected<fb::prepared_package_t, std::string> prepared =
            fb::prepare_package(sandbox / folder, index, false);
        return prepared ? std::string() : prepared.error();
    };
    const auto with_description = [&](const std::string_view folder,
                                      const std::string_view description) {
        sandbox.library(folder, folder, "1.0.0", "", { { "code.fb", "// code\n" } });
        std::string toml = sandbox.read(std::string(folder) + "/project.toml");
        toml.replace(toml.find("\"A test package\""), 16, description);
        sandbox.write(std::string(folder) + "/project.toml", toml);
    };

    // resolve-9: a description's 300 characters are code points, on one line.
    std::string wide;
    for (int i = 0; i < 300; ++i) {
        wide += "\xc3\xa9";
    }

    with_description("wide", "\"" + wide + "\"");
    CHECK(prepared_error("wide").empty());
    with_description("wider", "\"" + wide + "e\"");
    CHECK(prepared_error("wider") == "[project]'s description is longer than 300 characters");
    // As the registry counts them, in UTF-16, a code point past U+FFFF is two.
    std::string faces;
    for (int i = 0; i < 151; ++i) {
        faces += "\xf0\x9f\x98\x80";
    }

    with_description("faces", "\"" + faces + "\"");
    CHECK(prepared_error("faces") == "[project]'s description is longer than 300 characters");
    with_description("lines", R"("line one\nline two")");
    CHECK(prepared_error("lines") == "[project]'s description is a line, without line breaks");
    with_description("returns", R"("line one\rline two")");
    CHECK(prepared_error("returns").contains("without line breaks"));

    // archive-6: what fb would refuse to unpack isn't published: more than 8 times 10 MB, and
    // more files than the registry's.
    sandbox.library("big", "big", "1.0.0", "", { { "code.fb", "// code\n" } });
    sandbox.write("big/data/zeros.bin", std::string(std::size_t{ 80'000'001 }, '\0'));
    CHECK(prepared_error("big") == "big-1.0.0.fbpkg unpacks to 80.0 MB, more than the 80 MB fb "
                                   "unpacks of an archive of its kind");
    sandbox.library("many", "many", "1.0.0", "", { { "code.fb", "// code\n" } });
    for (std::size_t i = 0; i < fb::most_archive_files; ++i) {
        sandbox.write(std::format("many/data/{}", i), "");
    }

    CHECK(prepared_error("many").contains("more than the registry's 20000"));

    // The registry's reserved names, with or without their '_'s, as docs/registry-api.md has them.
    for (const std::string_view reserved :
         { "std", "fb", "futureblitz", "future_blitz", "apt", "rpm", "releases", "re_leases" }) {
        INFO(reserved);
        CHECK(fb::registry_name_error(reserved).value_or("").contains("keeps for FutureBlitz"));
    }

    CHECK_FALSE(fb::registry_name_error("apt_get"));
    CHECK(fb::registry_name_error("Physics"));
    CHECK(fb::registry_name_error(std::string(65, 'a')));
    CHECK_FALSE(fb::registry_name_error(std::string(64, 'a')));

    // commands-6: only a registry on disk limits a target's archive; one on the network knows
    // each package's own limit.
    CHECK(fb::target_size_error("x.fbpkg", 200'000'000, true) == std::nullopt);
    CHECK(fb::target_size_error("x.fbpkg", 200'000'001, true) ==
          "x.fbpkg is 200.0 MB, more than a registry on disk's 200 MB");
    CHECK(fb::target_size_error("x.fbpkg", 900'000'000, false) == std::nullopt);
}

TEST_CASE("a GitHub workflow publishes with trusted publishing's token", "[registry-api]") {
    const fb::testing::registry_server_t server;
    const fb::registry_session_t anonymous = session_of(server, "");
    CHECK_FALSE(fb::trusted_publishing_token(anonymous));

    const std::string base = server.index().substr(0, server.index().size() - 6);
    const fb::testing::environment_t url("ACTIONS_ID_TOKEN_REQUEST_URL",
                                         base + "github/oidc?api-version=2.0");
    const fb::testing::environment_t request("ACTIONS_ID_TOKEN_REQUEST_TOKEN", "request-token");
    // What trusted publishing gives: its token, "none" outside a workflow, or "error: ...".
    const auto token_of = [&] {
        const std::optional<std::expected<std::string, std::string>> token =
            fb::trusted_publishing_token(anonymous);
        if (!token) {
            return std::string("none");
        }

        return token->has_value() ? token->value() : "error: " + token->error();
    };
    const std::string token = token_of();
    CHECK(token == "fbr_trusted");

    const fb::testing::sandbox_t sandbox;
    library(sandbox, server, "shapes", "0.3.0", "", "// shapes\n");
    const fb::registry_session_t trusted = session_of(server, token);
    REQUIRE(publish(sandbox, trusted, "shapes"));
    REQUIRE(fb::revoke_token(trusted));
    CHECK(fb::yank_version("shapes", "0.3.0", false, trusted).error().contains("isn't it"));

    const fb::testing::environment_t wrong("ACTIONS_ID_TOKEN_REQUEST_TOKEN", "nope");
    const std::string refused = token_of();
    CHECK(refused.starts_with("error: "));
    CHECK(refused.contains("id-token: write"));
}

TEST_CASE("an environment variable's value is read as it is, not as a path", "[registry-api]") {
    const std::string value = "http://index.localhost:8787//x\\y \xc3\xa9";
    const fb::testing::environment_t set("FB_TEST_VALUE", value);
    CHECK(fb::environment_variable("FB_TEST_VALUE") == value);
    const fb::testing::environment_t unset("FB_TEST_VALUE", std::nullopt);
    CHECK_FALSE(fb::environment_variable("FB_TEST_VALUE"));
}

TEST_CASE("a credentials file keeps each registry's token", "[registry-api]") {
    const fb::testing::sandbox_t sandbox;
    const std::unique_ptr<fb::credential_store_t> store =
        fb::file_credential_store(sandbox / "home/credentials.json");
    const std::string key = fb::credential_key("https://index.futureblitz.dev/");
    CHECK(key == "futureblitz:https://index.futureblitz.dev/");
    CHECK(kept(*store, key) == "none");
    REQUIRE(store->set(key, "fbr_one"));
    REQUIRE(store->set("futureblitz:other", "fbr_two"));
    CHECK(kept(*store, key) == "fbr_one");
    REQUIRE(store->set(key, "fbr_three"));
    CHECK(kept(*fb::file_credential_store(sandbox / "home/credentials.json"), key) == "fbr_three");
    CHECK(store->remove(key) == true);
    CHECK(store->remove(key) == false);
    CHECK(kept(*store, "futureblitz:other") == "fbr_two");

    const fb::testing::environment_t file("FB_CREDENTIALS_FILE",
                                          fb::to_utf8(sandbox / "home/credentials.json"));
    CHECK(kept(*fb::credential_store(), "futureblitz:other") == "fbr_two");
    sandbox.write("home/credentials.json", "[1]");
    CHECK(kept(*store, key).contains("isn't a JSON object of tokens"));

    // commands-7: an empty file, as one made private first is, has no tokens yet.
    sandbox.write("home/credentials.json", "");
    CHECK(kept(*store, key) == "none");
    REQUIRE(store->set(key, "fbr_four"));
    CHECK(kept(*store, key) == "fbr_four");

    // A token is printable ASCII, and one that isn't leaves nothing behind.
    for (const std::string_view wrong : { "fbr_\xff\xfe", "\tfbr_x", "fbr x", "" }) {
        CHECK_FALSE(store->set(key, wrong).error().empty());
        CHECK(fb::token_error(wrong));
    }

    CHECK_FALSE(fb::token_error("fbr_abc-DEF.123"));
    std::vector<std::string> left;
    for (const auto& entry : std::filesystem::directory_iterator(sandbox / "home")) {
        left.push_back(fb::to_utf8(entry.path().filename()));
    }

    CHECK(left == std::vector<std::string>{ "credentials.json" });
    CHECK(kept(*store, key) == "fbr_four");
#ifndef _WIN32
    // commands-8: only its owner can read it, from the start.
    const auto permissions =
        std::filesystem::status(sandbox / "home/credentials.json").permissions();
    CHECK((permissions & std::filesystem::perms::all) ==
          (std::filesystem::perms::owner_read | std::filesystem::perms::owner_write));
#endif
}

// The OS's own store, which a person runs by hand, since it changes the machine's: it keeps a
// token, reads it back, and removes it.
TEST_CASE("the OS's credential store keeps a token", "[.credentials]") {
    const std::unique_ptr<fb::credential_store_t> store = fb::os_credential_store();
    const std::string key = "futureblitz:test-only:" + fb::sha256_hex("fb's own test");
    const std::expected<void, std::string> set = store->set(key, "fbr_os_test");
    INFO((set ? std::string() : set.error()));
    REQUIRE(set);
    CHECK(kept(*store, key) == "fbr_os_test");
    REQUIRE(store->set(key, "fbr_os_test_2"));
    CHECK(kept(*store, key) == "fbr_os_test_2");
    CHECK(store->remove(key) == true);
    CHECK(kept(*store, key) == "none");
}
