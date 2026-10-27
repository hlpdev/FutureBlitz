// The ACME tests' CA, driven by a client of its own here: an ES256 key through PSA, JWSes, an
// order proved by HTTP-01, and a CSR, whose certificate chains to the CA's root; and the
// requests it refuses, with RFC 8555's errors.

#include "acme_ca.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <mbedtls/pk.h>
#include <mbedtls/x509.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/x509_csr.h>
#include <optional>
#include <psa/crypto.h>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view base = "http://127.0.0.1:4001";

std::string base64url(const std::span<const std::uint8_t> bytes) {
    static constexpr std::string_view digits =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string text;
    for (std::size_t at = 0; at < bytes.size(); at += 3) {
        const std::size_t left = bytes.size() - at;
        std::uint32_t group = static_cast<std::uint32_t>(bytes[at]) << 16U;
        if (left > 1) {
            group |= static_cast<std::uint32_t>(bytes[at + 1]) << 8U;
        }

        if (left > 2) {
            group |= bytes[at + 2];
        }

        text += digits[(group >> 18U) & 63U];
        text += digits[(group >> 12U) & 63U];
        if (left > 1) {
            text += digits[(group >> 6U) & 63U];
        }

        if (left > 2) {
            text += digits[group & 63U];
        }
    }

    return text;
}

std::string base64url(const std::string_view text) {
    return base64url({ reinterpret_cast<const std::uint8_t*>(text.data()), text.size() });
}

/// The string member `key` of a JSON object's text, the first there is, or "".
std::string member(const std::string_view json, const std::string_view key) {
    const std::string start = std::format("\"{}\":\"", key);
    const std::size_t at = json.find(start);
    if (at == std::string_view::npos) {
        return {};
    }

    const std::size_t begin = at + start.size();
    return std::string(json.substr(begin, json.find('"', begin) - begin));
}

std::string header(const fb::test::acme_response_t& response, const std::string_view name) {
    for (const auto& [key, value] : response.headers) {
        if (key == name) {
            return value;
        }
    }

    return {};
}

/// A client of the CA, with an ES256 key, and the nonce the CA gave it last.
class client_t {
public:
    explicit client_t(fb::test::acme_ca_t& ca) : m_ca(ca) {
        psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
        psa_set_key_bits(&attributes, 256);
        psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_MESSAGE | PSA_KEY_USAGE_SIGN_HASH |
                                                 PSA_KEY_USAGE_EXPORT);
        psa_set_key_algorithm(&attributes, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
        REQUIRE(psa_generate_key(&attributes, &m_key) == PSA_SUCCESS);
        std::array<std::uint8_t, 65> point{};
        std::size_t length = 0;
        REQUIRE(psa_export_public_key(m_key, point.data(), point.size(), &length) == PSA_SUCCESS);
        m_x = base64url(std::span(point).subspan(1, 32));
        m_y = base64url(std::span(point).subspan(33, 32));
        m_nonce = header(m_ca.handle({ .method = "HEAD", .path = "/nonce" }), "Replay-Nonce");
    }

    ~client_t() {
        (void)psa_destroy_key(m_key);
    }

    client_t(const client_t&) = delete;
    client_t& operator=(const client_t&) = delete;
    client_t(client_t&&) = delete;
    client_t& operator=(client_t&&) = delete;

    /// The JWS of `payload` to `path`, with its jwk, or its account's kid once it has one, signed
    /// by the key, or by a signature `broken` changes.
    [[nodiscard]] std::string jws(const std::string_view path, const std::string_view payload,
                                  const std::string_view nonce, const std::string_view url,
                                  const bool broken = false) const {
        const std::string owner =
            m_account.empty()
                ? std::format(R"("jwk":{{"crv":"P-256","kty":"EC","x":"{}","y":"{}"}})", m_x, m_y)
                : std::format(R"("kid":"{}")", m_account);
        const std::string protected_part = base64url(
            std::format(R"({{"alg":"ES256",{},"nonce":"{}","url":"{}"}})", owner, nonce,
                        url.empty() ? std::format("{}{}", base, path) : std::string(url)));
        const std::string payload_part = base64url(payload);
        const std::string input = protected_part + "." + payload_part;
        std::array<std::uint8_t, 64> signature{};
        std::size_t length = 0;
        REQUIRE(psa_sign_message(m_key, PSA_ALG_ECDSA(PSA_ALG_SHA_256),
                                 reinterpret_cast<const std::uint8_t*>(input.data()), input.size(),
                                 signature.data(), signature.size(), &length) == PSA_SUCCESS);
        if (broken) {
            signature[10] ^= 1U;
        }

        return std::format(R"({{"protected":"{}","payload":"{}","signature":"{}"}})",
                           protected_part, payload_part, base64url(signature));
    }

    /// The CA's answer to a POST of `payload` to `path`, which keeps the nonce it gives.
    fb::test::acme_response_t post(const std::string_view path, const std::string_view payload) {
        fb::test::acme_response_t response =
            m_ca.handle({ .method = "POST",
                          .path = std::string(path),
                          .body = jws(path, payload, m_nonce, "") });
        m_nonce = header(response, "Replay-Nonce");
        return response;
    }

    /// A key authorization for `token`: it and the key's JWK thumbprint, RFC 7638's.
    [[nodiscard]] std::string key_authorization(const std::string_view token) const {
        const std::string jwk =
            std::format(R"({{"crv":"P-256","kty":"EC","x":"{}","y":"{}"}})", m_x, m_y);
        std::array<std::uint8_t, 32> hash{};
        std::size_t length = 0;
        REQUIRE(psa_hash_compute(PSA_ALG_SHA_256, reinterpret_cast<const std::uint8_t*>(jwk.data()),
                                 jwk.size(), hash.data(), hash.size(), &length) == PSA_SUCCESS);
        return std::format("{}.{}", token, base64url(hash));
    }

    /// A CSR of a new key for `names`, in base64url.
    [[nodiscard]] static std::string csr(const std::vector<std::string>& names) {
        psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
        psa_set_key_bits(&attributes, 256);
        psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_EXPORT);
        psa_set_key_algorithm(&attributes, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
        mbedtls_svc_key_id_t key{};
        REQUIRE(psa_generate_key(&attributes, &key) == PSA_SUCCESS);
        mbedtls_pk_context pk;
        mbedtls_pk_init(&pk);
        REQUIRE(mbedtls_pk_copy_from_psa(key, &pk) == 0);
        std::vector<mbedtls_x509_san_list> sans(names.size());
        for (std::size_t i = 0; i < names.size(); ++i) {
            sans[i].node.type = MBEDTLS_X509_SAN_DNS_NAME;
            sans[i].node.san.unstructured_name.p =
                reinterpret_cast<unsigned char*>(const_cast<char*>(names[i].data()));
            sans[i].node.san.unstructured_name.len = names[i].size();
            sans[i].next = i + 1 < names.size() ? &sans[i + 1] : nullptr;
        }

        mbedtls_x509write_csr writer;
        mbedtls_x509write_csr_init(&writer);
        mbedtls_x509write_csr_set_key(&writer, &pk);
        mbedtls_x509write_csr_set_md_alg(&writer, MBEDTLS_MD_SHA256);
        REQUIRE(mbedtls_x509write_csr_set_subject_name(&writer, "CN=test") == 0);
        REQUIRE(mbedtls_x509write_csr_set_subject_alternative_name(&writer, sans.data()) == 0);
        std::vector<std::uint8_t> der(2048);
        const int written = mbedtls_x509write_csr_der(&writer, der.data(), der.size());
        mbedtls_x509write_csr_free(&writer);
        mbedtls_pk_free(&pk);
        (void)psa_destroy_key(key);
        REQUIRE(written > 0);
        return base64url(std::span(der).last(static_cast<std::size_t>(written)));
    }

    /// Makes its account, and keeps its URL as its kid.
    void join() {
        const fb::test::acme_response_t made =
            post("/account", R"({"termsOfServiceAgreed":true,"contact":["mailto:a@example.com"]})");
        REQUIRE(made.status == 201);
        m_account = header(made, "Location");
    }

    [[nodiscard]] const std::string& nonce() const {
        return m_nonce;
    }

    [[nodiscard]] const std::string& account() const {
        return m_account;
    }

private:
    fb::test::acme_ca_t& m_ca;
    mbedtls_svc_key_id_t m_key{};
    std::string m_x;
    std::string m_y;
    std::string m_nonce;
    std::string m_account;
};

/// The key authorizations a test's HTTP-01 responder answers with, by token.
std::map<std::string, std::string>& responses() {
    static std::map<std::string, std::string> answers;
    return answers;
}

std::optional<std::string> fetch(const std::string& name, const std::string& token) {
    if (name != "localhost" && name != "www.localhost") {
        return std::nullopt;
    }

    const auto found = responses().find(token);
    return found != responses().end() ? std::optional(found->second) : std::nullopt;
}

} // namespace

TEST_CASE("an account orders a certificate, proves its names by HTTP-01, and gets it", "[acme]") {
    REQUIRE(psa_crypto_init() == PSA_SUCCESS);
    fb::test::acme_ca_t ca{ std::string(base), fetch };
    const fb::test::acme_response_t directory = ca.handle({ .method = "GET", .path = "/dir" });
    CHECK(directory.status == 200);
    CHECK(member(directory.body, "newAccount") == std::format("{}/account", base));
    CHECK(member(directory.body, "newNonce") == std::format("{}/nonce", base));

    client_t client(ca);
    CHECK(!client.nonce().empty());
    client.join();
    CHECK(client.account() == std::format("{}/account/1", base));

    const fb::test::acme_response_t order =
        client.post("/order", R"({"identifiers":[{"type":"dns","value":"localhost"},)"
                              R"({"type":"dns","value":"www.localhost"}]})");
    REQUIRE(order.status == 201);
    CHECK(header(order, "Location") == std::format("{}/order/1", base));
    CHECK(member(order.body, "status") == "pending");
    CHECK(member(order.body, "finalize") == std::format("{}/order/1/finalize", base));

    // Each name's authorization has an HTTP-01 challenge, which the responder answers, and which
    // the CA checks the second time the authorization is asked about after it.
    for (const std::string_view path : { "/authz/1", "/authz/2" }) {
        const fb::test::acme_response_t authorization = client.post(path, "");
        REQUIRE(authorization.status == 200);
        CHECK(header(authorization, "Retry-After").empty());
        const std::string token = member(authorization.body, "token");
        responses()[token] = client.key_authorization(token);
        const std::string url = member(authorization.body, "url");
        const fb::test::acme_response_t challenge = client.post(url.substr(base.size()), "{}");
        CHECK(challenge.status == 200);
        CHECK(member(challenge.body, "status") == "processing");
        const fb::test::acme_response_t waiting = client.post(path, "");
        CHECK(member(waiting.body, "status") == "pending");
        CHECK(header(waiting, "Retry-After") == "1");
        CHECK(member(client.post(url.substr(base.size()), "{}").body, "status") == "processing");
        const fb::test::acme_response_t checked = client.post(path, "");
        CHECK(member(checked.body, "status") == "valid");
        CHECK(header(checked, "Retry-After").empty());
    }

    CHECK(member(client.post("/order/1", "").body, "status") == "ready");
    const fb::test::acme_response_t finalized = client.post(
        "/order/1/finalize",
        std::format(R"({{"csr":"{}"}})", client_t::csr({ "www.localhost", "localhost" })));
    REQUIRE(finalized.status == 200);
    CHECK(member(finalized.body, "status") == "valid");
    const std::string certificate_url = member(finalized.body, "certificate");
    CHECK(certificate_url == std::format("{}/certificate/1", base));

    // The chain: the certificate, for both names, which the root signed, and the root.
    const fb::test::acme_response_t chain = client.post("/certificate/1", "");
    REQUIRE(chain.status == 200);
    mbedtls_x509_crt certificates;
    mbedtls_x509_crt_init(&certificates);
    REQUIRE(mbedtls_x509_crt_parse(&certificates,
                                   reinterpret_cast<const unsigned char*>(chain.body.c_str()),
                                   chain.body.size() + 1) == 0);
    mbedtls_x509_crt root;
    mbedtls_x509_crt_init(&root);
    const std::string& root_pem = fb::test::acme_ca_t::root_pem();
    REQUIRE(mbedtls_x509_crt_parse(&root, reinterpret_cast<const unsigned char*>(root_pem.c_str()),
                                   root_pem.size() + 1) == 0);
    std::uint32_t flags = 0;
    CHECK(mbedtls_x509_crt_verify(&certificates, &root, nullptr, "www.localhost", &flags, nullptr,
                                  nullptr) == 0);
    CHECK(mbedtls_x509_crt_verify(&certificates, &root, nullptr, "localhost", &flags, nullptr,
                                  nullptr) == 0);
    CHECK(mbedtls_x509_crt_verify(&certificates, &root, nullptr, "other.localhost", &flags, nullptr,
                                  nullptr) != 0);
    mbedtls_x509_crt_free(&root);
    mbedtls_x509_crt_free(&certificates);

    // The same key finds its account again.
    client_t again(ca);
    const fb::test::acme_response_t existing =
        again.post("/account", R"({"onlyReturnExisting":true})");
    CHECK(existing.status == 400);
    CHECK(member(existing.body, "type") == "urn:ietf:params:acme:error:accountDoesNotExist");
}

TEST_CASE("the CA refuses what RFC 8555 has it refuse", "[acme]") {
    REQUIRE(psa_crypto_init() == PSA_SUCCESS);
    fb::test::acme_ca_t ca{ std::string(base), fetch };
    client_t client(ca);
    const auto type = [](const fb::test::acme_response_t& response) {
        return member(response.body, "type")
            .substr(std::string_view("urn:ietf:params:acme:error:").size());
    };

    // A nonce the CA didn't give, one used already, a URL that isn't the request's, and a
    // signature that isn't the key's.
    const std::string nonce = client.nonce();
    const fb::test::acme_response_t unknown =
        ca.handle({ .method = "POST",
                    .path = "/account",
                    .body = client.jws("/account", "{}", "made-up", "") });
    CHECK(unknown.status == 400);
    CHECK(type(unknown) == "badNonce");
    const std::string terms = R"({"termsOfServiceAgreed":true})";
    CHECK(ca.handle({ .method = "POST",
                      .path = "/account",
                      .body = client.jws("/account", terms, nonce, "") })
              .status == 201);
    CHECK(type(ca.handle({ .method = "POST",
                           .path = "/account",
                           .body = client.jws("/account", terms, nonce, "") })) == "badNonce");
    const std::string fresh =
        header(ca.handle({ .method = "HEAD", .path = "/nonce" }), "Replay-Nonce");
    CHECK(type(ca.handle({ .method = "POST",
                           .path = "/account",
                           .body = client.jws("/account", terms, fresh,
                                              std::format("{}/elsewhere", base)) })) ==
          "unauthorized");
    const std::string another =
        header(ca.handle({ .method = "HEAD", .path = "/nonce" }), "Replay-Nonce");
    CHECK(type(ca.handle({ .method = "POST",
                           .path = "/account",
                           .body = client.jws("/account", terms, another, "", true) })) ==
          "unauthorized");
    client_t stranger(ca);
    CHECK(type(stranger.post("/account", "{}")) == "userActionRequired");
    CHECK(ca.handle({ .method = "POST", .path = "/account", .body = "not json" }).status == 400);

    // A nonce it refuses, though it gave it, once, after which the nonce that came with the
    // refusal works.
    ca.refuse_nonce();
    client_t refused(ca);
    const fb::test::acme_response_t refusal = refused.post("/account", terms);
    CHECK(type(refusal) == "badNonce");
    CHECK(member(refusal.body, "detail") == "the CA refused the nonce");
    CHECK(refused.post("/account", terms).status == 201);

    // An order before its names are proved isn't ready; a wrong key authorization fails the
    // challenge, and with it the order; and a CSR for other names is refused.
    client_t joined(ca);
    joined.join();
    REQUIRE(
        joined.post("/order", R"({"identifiers":[{"type":"dns","value":"localhost"}]})").status ==
        201);
    CHECK(type(joined.post("/order/1/finalize",
                           std::format(R"({{"csr":"{}"}})", client_t::csr({ "localhost" })))) ==
          "orderNotReady");
    const std::string token = member(joined.post("/authz/1", "").body, "token");
    responses()[token] = "wrong";
    CHECK(member(joined.post("/challenge/1", "{}").body, "status") == "processing");
    CHECK(member(joined.post("/authz/1", "").body, "status") == "pending");
    CHECK(member(joined.post("/authz/1", "").body, "status") == "invalid");
    CHECK(member(joined.post("/challenge/1", "{}").body, "status") == "invalid");
    CHECK(member(joined.post("/authz/1", "").body, "detail") ==
          "the response at localhost isn't the key authorization");
    CHECK(member(joined.post("/order/1", "").body, "status") == "invalid");

    REQUIRE(
        joined.post("/order", R"({"identifiers":[{"type":"dns","value":"localhost"}]})").status ==
        201);
    const std::string second = member(joined.post("/authz/2", "").body, "token");
    responses()[second] = joined.key_authorization(second);
    CHECK(member(joined.post("/challenge/2", "{}").body, "status") == "processing");
    CHECK(member(joined.post("/authz/2", "").body, "status") == "pending");
    CHECK(member(joined.post("/authz/2", "").body, "status") == "valid");
    CHECK(member(joined.post("/order/2", "").body, "status") == "ready");
    CHECK(type(joined.post(
              "/order/2/finalize",
              std::format(R"({{"csr":"{}"}})", client_t::csr({ "other.localhost" })))) == "badCSR");
    CHECK(type(joined.post("/order/2/finalize", R"({"csr":"bm90IGEgY3Ny"})")) == "badCSR");
    CHECK(type(joined.post("/order", R"({"identifiers":[{"type":"ip","value":"127.0.0.1"}]})")) ==
          "unsupportedIdentifier");
    CHECK(type(joined.post("/nowhere", "")) == "malformed");
}
