#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// A certificate authority for the ACME tests, as Pebble is for Let's Encrypt's: enough of RFC
// 8555 for a client to make an account, order a certificate, prove its names by HTTP-01, and
// finalize the order with a CSR, which it issues from its own root. It answers requests a test
// server gives it, so it has no socket of its own, and checks a challenge through the server's
// fetch, as Pebble does, later: the second time its authorization is asked about, after a
// Retry-After of a second. Its accounts' keys are ES256's, P-256, which is what std.net.acme
// makes.
namespace fb::test {

/// A request to the CA: its method, its path from the CA's base URL, its body, and its
/// Content-Type, which a POST's JWS has as application/jose+json.
struct acme_request_t {
    std::string method;
    std::string path;
    std::string body;
    std::string content_type;
};

struct acme_response_t {
    int status = 200;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
};

/// What an HTTP-01 challenge's check reads: the body of
/// http://<name>/.well-known/acme-challenge/<token>, from the test's own port, or nothing when
/// that doesn't answer.
using acme_fetch_t =
    std::function<std::optional<std::string>(const std::string& name, const std::string& token)>;

/// The body of `response`, an HTTP/1.x response of 200, as an HTTP-01 check reads it: by its
/// Content-Length, in chunks for Transfer-Encoding: chunked, or else to the end; or nothing for
/// another status or a response cut short.
[[nodiscard]] std::optional<std::string> body_of_ok(std::string_view response);

class acme_ca_t {
public:
    /// A CA whose URLs start with `base`, like "http://127.0.0.1:4001", and whose challenges are
    /// checked through `fetch`.
    acme_ca_t(std::string base, acme_fetch_t fetch);
    ~acme_ca_t();
    acme_ca_t(const acme_ca_t&) = delete;
    acme_ca_t& operator=(const acme_ca_t&) = delete;
    acme_ca_t(acme_ca_t&&) = delete;
    acme_ca_t& operator=(acme_ca_t&&) = delete;

    /// The CA's answer to `request`.
    [[nodiscard]] acme_response_t handle(const acme_request_t& request);

    /// Refuses the next request's nonce, as badNonce, as a CA may any nonce, which a client tries
    /// again after.
    void refuse_nonce();

    /// Has the certificates it issues from then on be valid from `before` seconds ago to `after`
    /// seconds from now, as a renewal's test needs, or from 2020 to 2120 again for nothing.
    void validity(std::optional<std::pair<std::int64_t, std::int64_t>> span);

    /// The root its certificates chain to, as PEM, which a test trusts.
    [[nodiscard]] static const std::string& root_pem();

private:
    struct state_t;

    std::string m_base;
    acme_fetch_t m_fetch;
    std::unique_ptr<state_t> m_state;
};

} // namespace fb::test
