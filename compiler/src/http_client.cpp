// fb's HTTP for registries: libcurl, the static one std.net.http is built on, over mbedTLS, which
// trusts the OS's roots as std.net.http's client does (the runtime's tls_roots.cpp).

#include "fb/http_client.hpp"

#include "fb/build_config.hpp"
#include "tls_roots.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <curl/curl.h>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <mbedtls/ssl.h>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace fb {
namespace {

/// How long a connection can take to open, and how long a transfer can go without a byte, in
/// seconds.
constexpr long connect_seconds = 30;
constexpr long stalled_seconds = 60;
constexpr long most_redirects = 10;

/// The most of a body that isn't a 200's fb keeps, from a file it was saved to: an error's words.
constexpr std::size_t most_error_body = std::size_t{ 1 } << 20U;

/// A response as libcurl gives it, piece by piece, and the file its body goes to, if it does,
/// which each response of a redirect's starts again.
struct received_t {
    http_response_t response;
    std::filesystem::path path;
    std::ofstream file;
};

// libcurl's callbacks take char*, though they only read it.
// NOLINTNEXTLINE(misc-const-correctness)
std::size_t on_body(char* data, const std::size_t size, const std::size_t count, void* given) {
    auto& received = *static_cast<received_t*>(given);
    if (!received.path.empty()) {
        if (!received.file.is_open()) {
            received.file.open(received.path, std::ios::binary);
        }

        received.file.write(data, static_cast<std::streamsize>(size * count));
        return received.file ? size * count : 0;
    }

    received.response.body.append(data, size * count);
    return size * count;
}

/// Each header line of each response, the last one's kept: a redirect's are dropped as the next
/// response's status line comes.
// NOLINTNEXTLINE(misc-const-correctness): libcurl's callbacks take char*
std::size_t on_header(char* data, const std::size_t size, const std::size_t count, void* given) {
    auto& received = *static_cast<received_t*>(given);
    const std::string_view line(data, size * count);
    if (line.starts_with("HTTP/")) {
        received.response.headers.clear();
        received.response.body.clear();
        if (received.file.is_open()) {
            received.file.close();
        }

        if (!received.path.empty()) {
            std::error_code ignored;
            std::filesystem::remove(received.path, ignored);
        }

        return size * count;
    }

    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos) {
        return size * count;
    }

    std::string name(line.substr(0, colon));
    for (char& c : name) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }

    std::string_view value = line.substr(colon + 1);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }

    while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ' ||
                              value.back() == '\t')) {
        value.remove_suffix(1);
    }

    received.response.headers[name] = std::string(value);
    return size * count;
}

/// libcurl's own check of a chain, which mbedTLS keeps in its configuration.
struct curl_check_t {
    int (*check)(void*, mbedtls_x509_crt*, int, std::uint32_t*) = nullptr;
    void* connection = nullptr;
};

/// mbedTLS's check of each certificate, as the OS checks it, then libcurl's.
// NOLINTNEXTLINE(misc-const-correctness)
int verify_as_os(void* check, mbedtls_x509_crt* certificate, const int depth,
                 std::uint32_t* flags) {
    (void)fb::runtime::os_verify(nullptr, certificate, depth, flags);
    const curl_check_t& curls = *static_cast<const curl_check_t*>(check);
    return curls.check != nullptr ? curls.check(curls.connection, certificate, depth, flags) : 0;
}

/// libcurl's mbedTLS configuration of a connection, which checks the chain as the OS does where
/// the roots mbedTLS has aren't all of them, as the runtime's http.cpp does.
CURLcode check_as_os(CURL* /*easy*/, void* config, void* check) {
    auto& ssl = *static_cast<mbedtls_ssl_config*>(config);
    curl_check_t& curls = *static_cast<curl_check_t*>(check);
    curls = { .check = ssl.MBEDTLS_PRIVATE(f_vrfy), .connection = ssl.MBEDTLS_PRIVATE(p_vrfy) };
    mbedtls_ssl_conf_verify(&ssl, verify_as_os, &curls);
    return CURLE_OK;
}

struct easy_deleter_t {
    void operator()(CURL* easy) const {
        curl_easy_cleanup(easy);
    }
};

struct list_deleter_t {
    void operator()(curl_slist* list) const {
        curl_slist_free_all(list);
    }
};

} // namespace

std::expected<http_response_t, std::string> send_http(const http_request_t& request) {
    static const CURLcode started = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (started != CURLE_OK) {
        return std::unexpected("libcurl couldn't start");
    }

    const std::unique_ptr<CURL, easy_deleter_t> easy(curl_easy_init());
    if (!easy) {
        return std::unexpected("libcurl couldn't start a transfer");
    }

    CURL* handle = easy.get();
    received_t received;
    curl_check_t curls;
    std::array<char, CURL_ERROR_SIZE> error{};
    const std::string agent = std::format("fb/{}", build_config::version);
    curl_easy_setopt(handle, CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(handle, CURLOPT_USERAGENT, agent.c_str());
    curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, error.data());
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(handle, CURLOPT_MAXREDIRS, most_redirects);
    // An https:// request is never redirected to http://.
    curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS_STR,
                     request.url.starts_with("https://") ? "https" : "http,https");
    curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(handle, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, connect_seconds);
    curl_easy_setopt(handle, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(handle, CURLOPT_LOW_SPEED_TIME, stalled_seconds);
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, static_cast<void*>(&received));
    curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(handle, CURLOPT_HEADERDATA, static_cast<void*>(&received));
    if (request.method == "HEAD") {
        curl_easy_setopt(handle, CURLOPT_NOBODY, 1L);
    } else if (request.method != "GET") {
        curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, request.method.c_str());
    }

    if (!request.body.empty() || (request.method != "GET" && request.method != "HEAD")) {
        curl_easy_setopt(handle, CURLOPT_POSTFIELDS, request.body.data());
        curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE_LARGE,
                         static_cast<curl_off_t>(request.body.size()));
    }

    // The OS's roots, which last as long as fb does.
    const std::string& roots = fb::runtime::os_roots();
    if (!roots.empty()) {
        curl_blob blob{
            .data = const_cast<char*>(roots.data()),
            .len = roots.size(),
            .flags = static_cast<unsigned>(CURL_BLOB_NOCOPY),
        };
        curl_easy_setopt(handle, CURLOPT_CAINFO_BLOB, &blob);
        curl_easy_setopt(handle, CURLOPT_PROXY_CAINFO_BLOB, &blob);
        curl_easy_setopt(handle, CURLOPT_SSL_CTX_FUNCTION, check_as_os);
        curl_easy_setopt(handle, CURLOPT_SSL_CTX_DATA, static_cast<void*>(&curls));
    }

    // "Expect: 100-continue" would wait on a server that doesn't answer it.
    curl_slist* fields = curl_slist_append(nullptr, "Expect:");
    for (const auto& [name, value] : request.headers) {
        fields = curl_slist_append(fields, std::format("{}: {}", name, value).c_str());
    }

    const std::unique_ptr<curl_slist, list_deleter_t> owned(fields);
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, fields);
    received.path = request.save_to;
    const CURLcode result = curl_easy_perform(handle);
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &received.response.status);
    if (!received.path.empty()) {
        received.file.close();
        std::error_code ignored;
        if (result == CURLE_OK && received.response.status == 200) {
            // An empty body is an empty file.
            if (!std::filesystem::exists(received.path, ignored)) {
                std::ofstream(received.path, std::ios::binary).close();
            }
        } else {
            // Another answer's body is its words, which go in the response.
            std::ifstream saved(received.path, std::ios::binary);
            received.response.body.resize(most_error_body);
            saved.read(received.response.body.data(),
                       static_cast<std::streamsize>(most_error_body));
            received.response.body.resize(static_cast<std::size_t>(saved.gcount()));
            saved.close();
            std::filesystem::remove(received.path, ignored);
        }
    }

    if (result != CURLE_OK) {
        const std::string_view why = error[0] != '\0' ? error.data() : curl_easy_strerror(result);
        return std::unexpected(std::format("{} couldn't be reached: {}", request.url, why));
    }

    return std::move(received.response);
}

std::string url_escape(const std::string_view value) {
    std::string out;
    for (const char c : value) {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        out += plain ? std::string(1, c)
                     : std::format("%{:02X}", static_cast<unsigned>(static_cast<unsigned char>(c)));
    }

    return out;
}

} // namespace fb
