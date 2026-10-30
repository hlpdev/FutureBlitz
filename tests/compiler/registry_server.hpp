#pragma once

// An HTTP server on 127.0.0.1 for the package manager's tests, which never reach the internet,
// and a registry over it: docs/registry-api.md's index, downloads, and the API fb uses, kept in
// memory, with one account whose token is test_token.

#include "fb/http_client.hpp"
#include "fb/manifest.hpp"
#include "fb/package_archive.hpp"
#include "fb/registry.hpp"
#include "fb/source.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef NOGDI
#define NOGDI
#endif
#include <winsock2.h>
// ws2tcpip.h needs winsock2.h's types first.
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace fb::testing {

// A client that's gone doesn't end the tests with SIGPIPE, on Linux.
#ifdef MSG_NOSIGNAL
inline constexpr int no_sigpipe = MSG_NOSIGNAL;
#else
inline constexpr int no_sigpipe = 0;
#endif

#ifdef _WIN32
using socket_t = SOCKET;
inline constexpr socket_t no_socket = INVALID_SOCKET;

inline void close_socket(const socket_t socket) {
    closesocket(socket);
}

inline int poll_sockets(pollfd* fds, const unsigned long count, const int timeout) {
    return WSAPoll(fds, count, timeout);
}
#else
using socket_t = int;
inline constexpr socket_t no_socket = -1;

inline void close_socket(const socket_t socket) {
    ::close(socket);
}

inline int poll_sockets(pollfd* fds, const nfds_t count, const int timeout) {
    return ::poll(fds, count, timeout);
}
#endif

/// An HTTP/1.1 server on 127.0.0.1, on a port of its own, that answers each request with
/// `handler`'s response, one connection at a time, each closed after its answer.
class http_server_t {
public:
    using handler_t = std::function<http_response_t(const http_request_t&)>;

    explicit http_server_t(handler_t handler) : m_handler(std::move(handler)) {
#ifdef _WIN32
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw std::runtime_error("WSAStartup failed");
        }
#endif
        m_listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API's way
        auto* any = reinterpret_cast<sockaddr*>(&address);
        socklen_t size = sizeof(address);
        if (m_listener == no_socket || ::bind(m_listener, any, sizeof(address)) != 0 ||
            ::listen(m_listener, 16) != 0 || ::getsockname(m_listener, any, &size) != 0) {
            throw std::runtime_error("the test's HTTP server couldn't listen on 127.0.0.1");
        }

        m_port = ntohs(address.sin_port);
        m_thread = std::thread([this] { serve(); });
    }

    http_server_t(const http_server_t&) = delete;
    http_server_t& operator=(const http_server_t&) = delete;
    http_server_t(http_server_t&&) = delete;
    http_server_t& operator=(http_server_t&&) = delete;

    ~http_server_t() {
        m_stopping = true;
        m_thread.join();
        close_socket(m_listener);
#ifdef _WIN32
        WSACleanup();
#endif
    }

    /// "http://127.0.0.1:<port>/".
    [[nodiscard]] std::string url() const {
        return std::format("http://127.0.0.1:{}/", m_port);
    }

private:
    handler_t m_handler;
    socket_t m_listener = no_socket;
    std::uint16_t m_port = 0;
    std::atomic<bool> m_stopping = false;
    std::thread m_thread;

    void serve() {
        while (!m_stopping) {
            pollfd waiting{ .fd = m_listener, .events = POLLIN, .revents = 0 };
            if (poll_sockets(&waiting, 1, 50) <= 0) {
                continue;
            }

            const socket_t connection = ::accept(m_listener, nullptr, nullptr);
            if (connection == no_socket) {
                continue;
            }

#ifdef SO_NOSIGPIPE
            // A client that's gone doesn't end the tests with SIGPIPE, on macOS.
            const int on = 1;
            ::setsockopt(connection, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif

            answer(connection);
            close_socket(connection);
        }
    }

    /// Reads one request from `connection` and writes its answer.
    void answer(const socket_t connection) {
        std::string received;
        std::size_t head_end = std::string::npos;
        std::size_t length = 0;
        std::array<char, 65536> chunk{};
        while (true) {
            if (head_end == std::string::npos) {
                head_end = received.find("\r\n\r\n");
                if (head_end != std::string::npos) {
                    length = content_length(received.substr(0, head_end));
                }
            }

            if (head_end != std::string::npos && received.size() >= head_end + 4 + length) {
                break;
            }

            const auto got = ::recv(connection, chunk.data(), static_cast<int>(chunk.size()), 0);
            if (got <= 0) {
                return;
            }

            received.append(chunk.data(), static_cast<std::size_t>(got));
        }

        http_request_t request;
        const std::string_view head = std::string_view(received).substr(0, head_end);
        const std::string_view line = head.substr(0, head.find("\r\n"));
        request.method = std::string(line.substr(0, line.find(' ')));
        const std::size_t target = line.find(' ') + 1;
        request.url = std::string(line.substr(target, line.find(' ', target) - target));
        for (std::size_t at = head.find("\r\n"); at != std::string_view::npos;) {
            const std::size_t next = head.find("\r\n", at + 2);
            const std::string_view field = head.substr(
                at + 2, next == std::string_view::npos ? std::string_view::npos : next - at - 2);
            if (const std::size_t colon = field.find(':'); colon != std::string_view::npos) {
                std::string name(field.substr(0, colon));
                std::ranges::transform(name, name.begin(), [](const char c) {
                    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
                });
                std::string_view value = field.substr(colon + 1);
                while (value.starts_with(' ')) {
                    value.remove_prefix(1);
                }

                request.headers.emplace_back(std::move(name), std::string(value));
            }

            at = next;
        }

        request.body = received.substr(head_end + 4, length);
        const http_response_t response = m_handler(request);
        std::string out = std::format("HTTP/1.1 {} Answer\r\nContent-Length: {}\r\nConnection: "
                                      "close\r\n",
                                      response.status, response.body.size());
        for (const auto& [name, value] : response.headers) {
            out += std::format("{}: {}\r\n", name, value);
        }

        out += "\r\n";
        if (request.method != "HEAD") {
            out += response.body;
        }

        for (std::size_t sent = 0; sent < out.size();) {
            const auto wrote = ::send(connection, out.data() + sent,
                                      static_cast<int>(out.size() - sent), no_sigpipe);
            if (wrote <= 0) {
                return;
            }

            sent += static_cast<std::size_t>(wrote);
        }
    }

    static std::size_t content_length(const std::string_view head) {
        std::string lower(head);
        std::ranges::transform(lower, lower.begin(), [](const char c) {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        });
        const std::size_t at = lower.find("\r\ncontent-length:");
        if (at == std::string::npos) {
            return 0;
        }

        return std::stoull(lower.substr(at + 17));
    }
};

/// The token the test registry's one account has.
inline constexpr std::string_view test_token = "fbr_test_token";

/// A registry over HTTP, as docs/registry-api.md has it, kept in memory: its index under
/// /index/, its archives under /dl/, and its API under /api/v1. It records each request.
class registry_server_t {
public:
    registry_server_t()
        : m_server([this](const http_request_t& request) { return handle(request); }) {}

    /// Its index's URL, as [registries] names it.
    [[nodiscard]] std::string index() const {
        return m_server.url() + "index/";
    }

    /// The requests it was sent, as "METHOD /path", in order.
    [[nodiscard]] std::vector<std::string> requests() const {
        const std::scoped_lock lock(m_mutex);
        return m_requests;
    }

    /// How many sign-in polls answer "pending" before one is approved.
    int pending_polls = 1;
    /// What a GitHub workflow's OIDC token is, which /trusted-publishing/token takes.
    std::string workflow_jwt = "header.payload.signature";
    /// The most an upload can be in one request, beyond which it takes parts of `part_size`.
    std::size_t part_size = 1024;

private:
    http_server_t m_server;
    mutable std::mutex m_mutex;
    std::vector<std::string> m_requests;
    std::map<std::string, std::string, std::less<>> m_index;
    std::map<std::string, std::string, std::less<>> m_files;
    std::map<std::string, std::string, std::less<>> m_uploads;
    std::map<std::string, std::map<int, std::string>, std::less<>> m_parts;
    std::map<std::string, std::vector<std::string>, std::less<>> m_owners;
    int m_polls = 0;
    std::set<std::string, std::less<>> m_revoked;

    static http_response_t json(const long status, const nlohmann::json& body) {
        return {
            .status = status,
            .headers = { { "content-type", "application/json" } },
            .body = body.dump(),
        };
    }

    static http_response_t error(const long status, const std::string_view code,
                                 const std::string_view detail) {
        return json(status, { { "errors", { { { "code", code }, { "detail", detail } } } } });
    }

    static std::string header(const http_request_t& request, const std::string_view name) {
        for (const auto& [field, value] : request.headers) {
            if (field == name) {
                return value;
            }
        }

        return "";
    }

    [[nodiscard]] bool authorized(const http_request_t& request) const {
        const std::string token = header(request, "authorization");
        return token == std::format("Bearer {}", test_token) ||
               (token == "Bearer fbr_trusted" && !m_revoked.contains("fbr_trusted"));
    }

    http_response_t handle(const http_request_t& request) {
        const std::scoped_lock lock(m_mutex);
        m_requests.push_back(std::format("{} {}", request.method, request.url));
        const std::string_view path = request.url;
        if (path == "/index/config.json") {
            return json(200, {
                                 { "v", 1 },
                                 { "dl", m_server.url() + "dl/{name}/{version}/{file}" },
                                 { "api", m_server.url() + "api/v1" },
                                 { "oidc_audience", "test.futureblitz.dev" },
                             });
        }

        if (path.starts_with("/index/")) {
            const auto found = m_index.find(path.substr(7));
            if (found == m_index.end()) {
                return error(404, "not_found", "no such package");
            }

            const std::string etag = std::format("\"{}\"", sha256_hex(found->second).substr(0, 16));
            if (header(request, "if-none-match") == etag) {
                return { .status = 304, .headers = { { "etag", etag } } };
            }

            return { .status = 200, .headers = { { "etag", etag } }, .body = found->second };
        }

        // GitHub's OIDC token service, as a workflow's ACTIONS_ID_TOKEN_REQUEST_URL is.
        if (path.starts_with("/github/oidc")) {
            if (header(request, "authorization") != "Bearer request-token" ||
                !path.contains("audience=test.futureblitz.dev")) {
                return error(403, "forbidden", "no id-token permission");
            }

            return json(200, { { "value", workflow_jwt } });
        }

        if (path.starts_with("/dl/")) {
            const auto found = m_files.find(path.substr(4));
            return found != m_files.end() ? http_response_t{ .status = 200, .body = found->second }
                                          : error(404, "not_found", "no such archive");
        }

        if (!path.starts_with("/api/v1/")) {
            return error(404, "not_found", "nothing's here");
        }

        return api(request, path.substr(7));
    }

    http_response_t api(const http_request_t& request, const std::string_view path) {
        const nlohmann::json body = nlohmann::json::parse(request.body, nullptr, false);
        if (path == "/cli-logins" && request.method == "POST") {
            m_polls = 0;
            return json(201, {
                                 { "id", "login1" },
                                 { "secret", "s3cret" },
                                 { "code", "WDJB-MJHT" },
                                 { "url", m_server.url() + "cli-login/login1" },
                                 { "interval", 1 },
                             });
        }

        if (path == "/cli-logins/login1/poll") {
            if (!body.is_object() || body.value("secret", "") != "s3cret") {
                return error(410, "expired", "no such sign-in");
            }

            if (m_polls++ < pending_polls) {
                return json(202, { { "status", "pending" } });
            }

            return json(200, { { "status", "approved" }, { "token", test_token } });
        }

        if (path == "/trusted-publishing/token") {
            if (!body.is_object() || body.value("token", "") != workflow_jwt) {
                return error(401, "unauthorized", "that's no workflow's token");
            }

            m_revoked.erase("fbr_trusted");
            return json(201, { { "token", "fbr_trusted" } });
        }

        if (path.starts_with("/search")) {
            nlohmann::json packages = nlohmann::json::array();
            const std::string words(
                path.substr(path.find("q=") + 2, path.find('&') - path.find("q=") - 2));
            for (const auto& [name, lines] : m_index) {
                if (name.contains(words)) {
                    packages.push_back({
                        { "name", name },
                        { "max_version", newest(lines) },
                        { "description", "A test package" },
                    });
                }
            }

            return json(200, { { "packages", packages }, { "total", packages.size() } });
        }

        if (!authorized(request)) {
            return error(401, "unauthorized", "a token is needed, and that one isn't it");
        }

        if (path == "/tokens/current" && request.method == "DELETE") {
            m_revoked.insert("fbr_trusted");
            return { .status = 204 };
        }

        if (path.starts_with("/uploads/")) {
            return upload(request, path.substr(9));
        }

        if (path.starts_with("/packages/")) {
            return package(request, path.substr(10), body);
        }

        return error(404, "not_found", "nothing's here");
    }

    http_response_t upload(const http_request_t& request, const std::string_view path) {
        const std::string sha(path.substr(0, path.find('/')));
        const std::string_view rest = path.substr(sha.size());
        if (rest.empty() && (request.method == "HEAD" || request.method == "GET")) {
            return m_uploads.contains(sha) ? json(200, { { "sha256", sha } })
                                           : error(404, "not_found", "not staged");
        }

        if (rest.empty() && request.method == "PUT") {
            if (sha256_hex(request.body) != sha) {
                return error(422, "checksum", "the bytes aren't that SHA-256's");
            }

            m_uploads[sha] = request.body;
            return json(201, { { "sha256", sha }, { "size", request.body.size() } });
        }

        if (rest == "/parts" && request.method == "POST") {
            m_parts[sha].clear();
            return json(201, { { "upload_id", "parts1" }, { "part_size", part_size } });
        }

        if (rest == "/parts/parts1/complete") {
            std::string whole;
            for (const auto& [number, bytes] : m_parts[sha]) {
                whole += bytes;
            }

            if (sha256_hex(whole) != sha) {
                return error(422, "checksum", "the parts aren't that SHA-256's");
            }

            m_uploads[sha] = whole;
            return json(201, { { "sha256", sha }, { "size", whole.size() } });
        }

        if (rest.starts_with("/parts/parts1/") && request.method == "PUT") {
            const int number = std::stoi(std::string(rest.substr(14)));
            m_parts[sha][number] = request.body;
            return json(200, { { "part", number }, { "etag", std::format("etag{}", number) } });
        }

        return error(404, "not_found", "nothing's here");
    }

    /// A package's versions, yanks, and owners.
    http_response_t package(const http_request_t& request, const std::string_view path,
                            const nlohmann::json& body) {
        const std::string name(path.substr(0, path.find('/')));
        const std::string_view rest = path.substr(name.size());
        if (rest == "/versions" && request.method == "POST") {
            return publish(name, body);
        }

        if (rest == "/owners") {
            std::vector<std::string>& owners = m_owners[name];
            if (request.method == "GET") {
                nlohmann::json listed = nlohmann::json::array();
                for (const std::string& owner : owners) {
                    listed.push_back({ { "kind", "user" }, { "name", owner } });
                }

                return json(200, { { "owners", listed }, { "invites", nlohmann::json::array() } });
            }

            const std::string user = body.is_object() ? body.value("user", "") : "";
            if (request.method == "POST") {
                return json(202, { { "id", "invite1" }, { "kind", "owner" } });
            }

            if (owners.size() == 1 && owners.front() == user) {
                return error(409, "last_owner", "a package keeps one owner at least");
            }

            std::erase(owners, user);
            return { .status = 204 };
        }

        if (rest.starts_with("/versions/") &&
            (rest.ends_with("/yank") || rest.ends_with("/unyank"))) {
            const bool yank = rest.ends_with("/yank");
            const std::string_view version = rest.substr(10, rest.rfind('/') - 10);
            return set_yanked(name, version, yank);
        }

        return error(404, "not_found", "nothing's here");
    }

    http_response_t publish(const std::string& name, const nlohmann::json& body) {
        if (!body.is_object() || !body.contains("archives") ||
            !body["archives"].contains("source")) {
            return error(400, "bad_request", "a publish names its archives");
        }

        const std::string version = body.value("version", "");
        const std::string source = body["archives"]["source"].value("sha256", "");
        if (!m_uploads.contains(source)) {
            return error(400, "bad_request", "the source archive wasn't uploaded");
        }

        // The index line comes from the archive's own project.toml.
        const auto files = read_archive(m_uploads[source], most_source_unpacked);
        if (!files) {
            return error(422, "archive", files.error());
        }

        const auto project = std::ranges::find(*files, "project.toml", &archive_file_t::path);
        if (project == files->end()) {
            return error(422, "manifest", "there's no project.toml");
        }

        const source_file_t file(file_id_t{}, "project.toml", project->bytes);
        diagnostic_bag_t diagnostics;
        const std::optional<manifest_t> manifest = read_manifest(file, diagnostics);
        if (!manifest || manifest->name != name || manifest->version != version) {
            return error(422, "manifest", "project.toml isn't the upload's");
        }

        std::string& lines = m_index[name];
        if (lines.contains(std::format(R"("version":"{}")", version))) {
            return error(409, "version_exists",
                         std::format("{} {} is already published", name, version));
        }

        index_entry_t entry{ .name = name, .version = version };
        if (manifest->fb) {
            entry.fb = manifest->fb->text;
        }

        for (const dependency_t& dependency : manifest->dependencies) {
            entry.dependencies.push_back(
                { .name = dependency.name, .req = dependency.range ? dependency.range->text : "" });
        }

        for (const auto& [key, archive] : body["archives"].items()) {
            const std::string sha = archive.value("sha256", "");
            if (!m_uploads.contains(sha)) {
                return error(400, "bad_request", std::format("{} wasn't uploaded", key));
            }

            const archive_ref_t ref{ .sha256 = sha, .size = m_uploads[sha].size() };
            m_files[std::format("{}/{}/{}", name, version,
                                archive_name(name, version, key == "source" ? "" : key))] =
                m_uploads[sha];
            if (key == "source") {
                entry.source = ref;
            } else {
                entry.targets.emplace(key, ref);
            }
        }

        if (body.contains("docs") && body["docs"].is_object() &&
            !m_uploads.contains(body["docs"].value("sha256", ""))) {
            return error(400, "docs", "the docs weren't uploaded");
        }

        lines += index_line(entry) + "\n";
        m_owners.try_emplace(name, std::vector<std::string>{ "tester" });
        return json(201, {
                             { "name", name },
                             { "version", version },
                             { "warnings", { "the readme has no title" } },
                         });
    }

    http_response_t set_yanked(const std::string& name, const std::string_view version,
                               const bool yanked) {
        const auto found = m_index.find(name);
        if (found == m_index.end()) {
            return error(404, "not_found", "no such package");
        }

        auto entries = parse_index(found->second, name);
        std::string out;
        bool there = false;
        for (index_entry_t& entry : *entries) {
            if (entry.version == version) {
                entry.yanked = yanked;
                there = true;
            }

            out += index_line(entry) + "\n";
        }

        if (!there) {
            return error(404, "not_found", "no such version");
        }

        found->second = std::move(out);
        return json(200, { { "version", version }, { "yanked", yanked } });
    }

    static std::string newest(const std::string_view lines) {
        std::string out;
        for (const auto part : std::views::split(lines, '\n')) {
            const nlohmann::json line =
                nlohmann::json::parse(std::string_view(part.begin(), part.end()), nullptr, false);
            if (line.is_object()) {
                out = line.value("version", "");
            }
        }

        return out;
    }
};

} // namespace fb::testing
