// The runtime's TLS below what std.net.tls checks first: two connections through memory, made
// with the fb_tls_ functions, whose handshakes fail as only a peer that isn't std's, or a
// configuration std.net.tls panics for, makes them, and a client of an IP address, which goes
// as no server_name.
#include "fb/runtime.hpp"
#include "fb/tls.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <ios>
#include <iterator>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

std::string read_file(const char* path) {
    std::ifstream file(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}

/// A certificate's DER, from PEM.
std::vector<std::uint8_t> der_of(const std::string& pem) {
    mbedtls_x509_crt certificate;
    mbedtls_x509_crt_init(&certificate);
    REQUIRE(mbedtls_x509_crt_parse(&certificate,
                                   reinterpret_cast<const unsigned char*>(pem.c_str()),
                                   pem.size() + 1) == 0);
    std::vector<std::uint8_t> der(certificate.raw.p, certificate.raw.p + certificate.raw.len);
    mbedtls_x509_crt_free(&certificate);
    return der;
}

/// An array of the bytes, as std.net.tls passes the runtime a UInt8[].
fb_array_t array_of(std::vector<std::uint8_t>& bytes) {
    return {
        .header = {},
        .count = static_cast<std::int64_t>(bytes.size()),
        .capacity = static_cast<std::int64_t>(bytes.size()),
        .data = bytes.data(),
    };
}

std::string text_of(fb_string_t* string) {
    std::string text(reinterpret_cast<const char*>(string + 1),
                     static_cast<std::size_t>(string->length));
    fb_string_release(string);
    return text;
}

/// A configuration, a client's or a server's, which trusts only the test CA, and which a
/// connection keeps.
class config_t {
public:
    explicit config_t(const bool server) : m_config(fb_tls_config_new(server ? 1 : 0)) {
        REQUIRE(psa_crypto_init() == PSA_SUCCESS);
        std::vector<std::uint8_t> root = der_of(read_file(FB_TEST_TLS_CA));
        const fb_array_t der = array_of(root);
        fb_tls_config_trust_own(m_config, 1);
        REQUIRE(fb_tls_config_trust(m_config, &der) == 0);
    }

    config_t(const config_t&) = delete;
    config_t& operator=(const config_t&) = delete;

    ~config_t() {
        fb_tls_config_free(m_config);
    }

    /// Has it give the test server's certificate and key.
    void identify() {
        std::vector<std::uint8_t> certificate = der_of(read_file(FB_TEST_TLS_CERTIFICATE));
        const fb_array_t der = array_of(certificate);
        REQUIRE(fb_tls_config_chain(m_config, &der) == 0);
        fb_string_t* key = fb_string_from_cstring(read_file(FB_TEST_TLS_KEY).c_str());
        REQUIRE(fb_tls_config_key(m_config, key) == 0);
        fb_string_release(key);
    }

    [[nodiscard]] void* get() const {
        return m_config;
    }

private:
    void* m_config;
};

/// A connection of a configuration's, to `host`, with `address`, its bytes for an IP address.
class tls_t {
public:
    tls_t(const config_t& config, const std::string& host, std::vector<std::uint8_t> address = {})
        : m_address(std::move(address)) {
        fb_string_t* name = fb_string_from_cstring(host.c_str());
        const fb_array_t bytes = array_of(m_address);
        m_tls = fb_tls_new(config.get(), name, &bytes);
        fb_string_release(name);
    }

    tls_t(const tls_t&) = delete;
    tls_t& operator=(const tls_t&) = delete;

    ~tls_t() {
        fb_tls_free(m_tls);
    }

    /// What it has for the other side.
    std::vector<std::uint8_t> take() {
        std::vector<std::uint8_t> records(static_cast<std::size_t>(fb_tls_pending(m_tls)));
        (void)fb_tls_take(m_tls, records.data(), static_cast<std::int64_t>(records.size()));
        return records;
    }

    void feed(const std::vector<std::uint8_t>& records) {
        if (!records.empty()) {
            fb_tls_feed(m_tls, records.data(), static_cast<std::int64_t>(records.size()));
        }
    }

    [[nodiscard]] void* get() const {
        return m_tls;
    }

    [[nodiscard]] std::string message() const {
        return text_of(fb_tls_message(m_tls));
    }

private:
    std::vector<std::uint8_t> m_address;
    void* m_tls = nullptr;
};

/// What each side's handshake ends with, 0 or -1, once neither has more to say, and what the
/// client sent first, its ClientHello.
struct shaken_t {
    std::int32_t client = 1;
    std::int32_t server = 1;
    std::vector<std::uint8_t> hello;
};

shaken_t shake(tls_t& client, tls_t& server) {
    shaken_t shaken;
    for (int round = 0; round < 20 && (shaken.client == 1 || shaken.server == 1); ++round) {
        if (shaken.client == 1) {
            shaken.client = fb_tls_handshake(client.get());
        }

        const std::vector<std::uint8_t> sent = client.take();
        if (round == 0) {
            shaken.hello = sent;
        }

        server.feed(sent);
        if (shaken.server == 1) {
            shaken.server = fb_tls_handshake(server.get());
        }

        client.feed(server.take());
    }

    return shaken;
}

bool holds(const std::vector<std::uint8_t>& bytes, const std::string_view text) {
    return !std::ranges::search(bytes, text).empty();
}

} // namespace

TEST_CASE("a client whose certificate isn't for a TLS client is refused for its usage",
          "[runtime][tls]") {
    // std.net.tls's Identify panics for one, as the test server's, whose usage is serverAuth;
    // another TLS's client can send it.
    config_t server_config(true);
    server_config.identify();
    fb_tls_config_require_clients(server_config.get());
    CHECK(fb_tls_config_usable(server_config.get(), 1) == 0);
    CHECK(fb_tls_config_usable(server_config.get(), 0) == -1);
    config_t client_config(false);
    client_config.identify();
    tls_t client(client_config, "localhost");
    tls_t server(server_config, "");
    const shaken_t shaken = shake(client, server);
    CHECK(shaken.server == -1);
    CHECK(server.message() == "the client's certificate isn't for a TLS client");
}

TEST_CASE("mbedTLS's PSA statuses say what they are", "[runtime][tls]") {
    // A host past a server_name's 255 bytes, which std.net.tls panics for, is PSA's invalid
    // argument, which mbedtls_strerror reads as an HMAC_DRBG error.
    config_t client_config(false);
    tls_t long_host(client_config, std::string(300, 'a'));
    CHECK(fb_tls_handshake(long_host.get()) == -1);
    CHECK(long_host.message() == "mbedTLS failed with PSA's status -135");

    // Protocols past what a ClientHello holds, which std.net.tls panics for too.
    config_t offering(false);
    for (int i = 0; i < 70; ++i) {
        fb_string_t* protocol = fb_string_from_cstring(std::string(250, 'p').c_str());
        fb_tls_config_offer(offering.get(), protocol);
        fb_string_release(protocol);
    }

    tls_t big(offering, "localhost");
    CHECK(fb_tls_handshake(big.get()) == -1);
    CHECK(big.message() == "a handshake message this side made is bigger than mbedTLS's buffer");
}

TEST_CASE("a client of an IP address sends no server_name, and checks the address",
          "[runtime][tls]") {
    config_t server_config(true);
    server_config.identify();
    config_t client_config(false);
    {
        tls_t client(client_config, "localhost");
        tls_t server(server_config, "");
        const shaken_t shaken = shake(client, server);
        CHECK(shaken.client == 0);
        CHECK(shaken.server == 0);
        CHECK(holds(shaken.hello, "localhost"));
    }

    // RFC 6066: "Literal IPv4 and IPv6 addresses are not permitted in HostName." The test
    // server's certificate names 127.0.0.1 and ::1.
    for (const auto& [host, address] :
         std::vector<std::pair<std::string, std::vector<std::uint8_t>>>{
             { "127.0.0.1", { 127, 0, 0, 1 } },
             { "::1", { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 } } }) {
        tls_t client(client_config, host, address);
        tls_t server(server_config, "");
        const shaken_t shaken = shake(client, server);
        CHECK(shaken.client == 0);
        CHECK(shaken.server == 0);
        CHECK(!holds(shaken.hello, host));
    }

    tls_t other(client_config, "127.0.0.2", { 127, 0, 0, 2 });
    tls_t server(server_config, "");
    const shaken_t shaken = shake(other, server);
    CHECK(shaken.client == -1);
    CHECK(other.message() == "the server's certificate isn't for 127.0.0.2");
}
