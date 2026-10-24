// std.net.tls's TLS over mbedTLS, through memory: what comes from the stream a connection is on
// goes in with fb_tls_feed, and the records it makes come out with fb_tls_take, so a blocking
// stream and an Async one, a socket or anything else, are all the same to it.

#include "fb/tls.hpp"

#include "fb/runtime.hpp"
#include "tls_roots.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <limits>
#include <mbedtls/error.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509.h>
#include <mbedtls/x509_crt.h>
#include <mutex>
#include <psa/crypto.h>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

/// What fb_tls_handshake gives.
constexpr std::int32_t handshake_done = 0;
constexpr std::int32_t handshake_wants_input = 1;
constexpr std::int32_t handshake_failed = -1;
constexpr std::int32_t handshake_ended = -2;

/// What fb_tls_read gives besides a count, and 0 for close_notify.
constexpr std::int64_t read_wants_input = -1;
constexpr std::int64_t read_failed = -2;
constexpr std::int64_t read_cut_short = -3;

/// Starts PSA, which mbedTLS's TLS works through, once for every thread. A program that can't
/// have the OS's randomness can't go on safely, so it panics.
void start() {
    static const psa_status_t started = psa_crypto_init();
    if (started != PSA_SUCCESS) {
        fb_panic("std.net.tls can't start, since the OS gives no random bytes", nullptr);
    }
}

/// The OS's roots, read once, which every configuration that trusts them shares, since mbedTLS
/// only reads them. One mbedTLS can't read is left out.
mbedtls_x509_crt& os_chain() {
    static mbedtls_x509_crt chain;
    static std::once_flag read;
    std::call_once(read, [] {
        mbedtls_x509_crt_init(&chain);
        // mbedTLS reads PEM with the NUL after it.
        const std::string& pem = fb::runtime::os_roots();
        if (!pem.empty()) {
            (void)mbedtls_x509_crt_parse(
                &chain, reinterpret_cast<const unsigned char*>(pem.c_str()), pem.size() + 1);
        }
    });
    return chain;
}

std::string_view view_of(const fb_string_t* text) {
    return { reinterpret_cast<const char*>(text + 1), static_cast<std::size_t>(text->length) };
}

fb_string_t* string_of(const std::string_view text) {
    return fb_string_from_bytes(text.data(), static_cast<std::int64_t>(text.size()));
}

/// A client's or a server's configuration, which its connections share, and which is set up for
/// mbedTLS when the first of them is made.
struct config_t {
    std::atomic<std::int64_t> references = 1;
    bool server = false;
    bool own_roots = false;
    bool require_clients = false;
    bool has_key = false;
    mbedtls_x509_crt roots{};
    mbedtls_x509_crt chain{};
    mbedtls_pk_context key{};
    std::vector<std::string> protocols;
    /// The protocols' C strings, and a null after them, as mbedTLS takes them.
    std::vector<const char*> offered;
    mbedtls_ssl_config ssl{};
    std::once_flag set_up;
    /// What setting it up gave: 0, or mbedTLS's error.
    int setup = 0;
};

void retain(config_t* config) {
    config->references.fetch_add(1, std::memory_order_relaxed);
}

void release(config_t* config) {
    if (config->references.fetch_sub(1, std::memory_order_acq_rel) != 1) {
        return;
    }

    mbedtls_ssl_config_free(&config->ssl);
    mbedtls_x509_crt_free(&config->roots);
    mbedtls_x509_crt_free(&config->chain);
    mbedtls_pk_free(&config->key);
    delete config;
}

/// mbedTLS's configuration of what `config` has, or its error.
int set_up(config_t& config) {
    int result = mbedtls_ssl_config_defaults(
        &config.ssl, config.server ? MBEDTLS_SSL_IS_SERVER : MBEDTLS_SSL_IS_CLIENT,
        MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (result != 0) {
        return result;
    }

    // A client checks the server's certificate, and a server a client's only when it asks for
    // one.
    const bool verify = !config.server || config.require_clients;
    mbedtls_ssl_conf_authmode(&config.ssl,
                              verify ? MBEDTLS_SSL_VERIFY_REQUIRED : MBEDTLS_SSL_VERIFY_NONE);
    if (verify) {
        mbedtls_ssl_conf_ca_chain(&config.ssl, config.own_roots ? &config.roots : &os_chain(),
                                  nullptr);
    }

    if (config.has_key) {
        result = mbedtls_ssl_conf_own_cert(&config.ssl, &config.chain, &config.key);
        if (result != 0) {
            return result;
        }
    }

    if (!config.protocols.empty()) {
        for (const std::string& protocol : config.protocols) {
            config.offered.push_back(protocol.c_str());
        }

        config.offered.push_back(nullptr);
        result = mbedtls_ssl_conf_alpn_protocols(&config.ssl, config.offered.data());
    }

    return result;
}

/// A connection, whose records go through memory: `input`, from `read_at`, holds what came from
/// the other side, and `output`, from `taken`, what it has for the other side.
struct tls_t {
    config_t* config = nullptr;
    std::string host;
    mbedtls_ssl_context ssl{};
    /// What making it gave: 0, or mbedTLS's error, which its handshake fails with.
    int setup = 0;
    std::vector<unsigned char> input;
    std::size_t read_at = 0;
    /// Whether the stream it's on has ended.
    bool ended = false;
    std::vector<unsigned char> output;
    std::size_t taken = 0;
    /// Whether the other side's close_notify came.
    bool closed = false;
    /// Whether its handshake is done.
    bool handshaken = false;
    std::string message;
};

int send(void* context, const unsigned char* bytes, const std::size_t count) {
    auto& tls = *static_cast<tls_t*>(context);
    tls.output.insert(tls.output.end(), bytes, bytes + count);
    return static_cast<int>(count);
}

int receive(void* context, unsigned char* into, const std::size_t capacity) {
    auto& tls = *static_cast<tls_t*>(context);
    const std::size_t left = tls.input.size() - tls.read_at;
    if (left == 0) {
        return tls.ended ? 0 : MBEDTLS_ERR_SSL_WANT_READ;
    }

    const std::size_t count = std::min(left, capacity);
    std::memcpy(into, tls.input.data() + tls.read_at, count);
    tls.read_at += count;
    if (tls.read_at == tls.input.size()) {
        tls.input.clear();
        tls.read_at = 0;
    }

    return static_cast<int>(count);
}

/// Each reason a certificate that mbedTLS checked isn't good enough, from its flags, after "the
/// server's certificate".
std::vector<std::string> reasons(const tls_t& tls, const std::uint32_t flags) {
    std::vector<std::string> found;
    if ((flags & MBEDTLS_X509_BADCERT_NOT_TRUSTED) != 0) {
        found.emplace_back("isn't signed by a root this side trusts");
    }

    if ((flags & MBEDTLS_X509_BADCERT_EXPIRED) != 0) {
        found.emplace_back("has expired");
    }

    if ((flags & MBEDTLS_X509_BADCERT_FUTURE) != 0) {
        found.emplace_back("isn't valid yet");
    }

    if ((flags & MBEDTLS_X509_BADCERT_CN_MISMATCH) != 0) {
        found.push_back(std::format("isn't for {}", tls.host));
    }

    if ((flags & MBEDTLS_X509_BADCERT_REVOKED) != 0) {
        found.emplace_back("is revoked");
    }

    if ((flags & (MBEDTLS_X509_BADCERT_KEY_USAGE | MBEDTLS_X509_BADCERT_EXT_KEY_USAGE |
                  MBEDTLS_X509_BADCERT_NS_CERT_TYPE)) != 0) {
        found.push_back(
            std::format("isn't for a TLS {}", tls.config->server ? "client" : "server"));
    }

    if ((flags & (MBEDTLS_X509_BADCERT_BAD_MD | MBEDTLS_X509_BADCERT_BAD_PK |
                  MBEDTLS_X509_BADCERT_BAD_KEY)) != 0) {
        found.emplace_back("is signed with a hash or a key TLS doesn't take");
    }

    return found;
}

/// Why `result`, mbedTLS's error, came of a step of `tls`.
std::string describe(const tls_t& tls, const int result) {
    const std::string_view other = tls.config->server ? "client" : "server";
    // mbedTLS sets every flag until a certificate is checked.
    std::uint32_t flags = mbedtls_ssl_get_verify_result(&tls.ssl);
    if (flags == std::numeric_limits<std::uint32_t>::max()) {
        flags = 0;
    }

    if ((flags & MBEDTLS_X509_BADCERT_MISSING) != 0) {
        return std::format("the {} sent no certificate", other);
    }

    if (result == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
        const std::vector<std::string> found = reasons(tls, flags);
        std::string message = std::format("the {}'s certificate", other);
        for (std::size_t i = 0; i < found.size(); ++i) {
            message += std::format("{}{}", i == 0 ? " " : ", and ", found[i]);
        }

        return found.empty() ? message + " isn't one this side takes" : message;
    }

    switch (result) {
    case MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE:
        return tls.handshaken ? std::format("the {} ended the connection with an alert", other)
                              : std::format("the {} refused the handshake", other);
    // A TLS 1.3 alert that refuses a handshake can come under keys the other side has moved
    // past, as mbedTLS's do, so it reads as a record that doesn't authenticate.
    case MBEDTLS_ERR_SSL_INVALID_MAC:
        return tls.handshaken
                   ? std::format("a record the {} sent doesn't authenticate, as when it refused "
                                 "this side's certificate",
                                 other)
                   : std::format("the {} refused the handshake, or sent a record that doesn't "
                                 "authenticate",
                                 other);
    case MBEDTLS_ERR_SSL_NO_APPLICATION_PROTOCOL:
        return "the client and the server have no application protocol in common";
    case MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE:
    case MBEDTLS_ERR_SSL_BAD_PROTOCOL_VERSION:
        return "the client and the server have no version of TLS or cipher suite in common";
    case MBEDTLS_ERR_SSL_INVALID_RECORD:
    case MBEDTLS_ERR_SSL_DECODE_ERROR:
    case MBEDTLS_ERR_SSL_UNEXPECTED_MESSAGE:
        return std::format("what the {} sent isn't TLS", other);
    default:
        break;
    }

    std::array<char, 256> text{};
    mbedtls_strerror(result, text.data(), text.size());
    return std::format("mbedTLS failed: {}", text.data());
}

tls_t& connection(void* tls) {
    return *static_cast<tls_t*>(tls);
}

config_t& configuration(void* config) {
    return *static_cast<config_t*>(config);
}

} // namespace

extern "C" {

void* fb_tls_config_new(const std::int32_t server) {
    start();
    auto* config = new config_t();
    config->server = server != 0;
    mbedtls_x509_crt_init(&config->roots);
    mbedtls_x509_crt_init(&config->chain);
    mbedtls_pk_init(&config->key);
    mbedtls_ssl_config_init(&config->ssl);
    return config;
}

void fb_tls_config_free(void* config) {
    release(static_cast<config_t*>(config));
}

void fb_tls_config_trust_own(void* config, const std::int32_t own) {
    configuration(config).own_roots = own != 0;
}

std::int32_t fb_tls_config_trust(void* config, const fb_array_t* der) {
    return mbedtls_x509_crt_parse_der(&configuration(config).roots,
                                      static_cast<const unsigned char*>(der->data),
                                      static_cast<std::size_t>(der->count)) == 0
               ? 0
               : -1;
}

void fb_tls_config_require_clients(void* config) {
    configuration(config).require_clients = true;
}

std::int32_t fb_tls_config_chain(void* config, const fb_array_t* der) {
    return mbedtls_x509_crt_parse_der(&configuration(config).chain,
                                      static_cast<const unsigned char*>(der->data),
                                      static_cast<std::size_t>(der->count)) == 0
               ? 0
               : -1;
}

std::int32_t fb_tls_config_key(void* config, const fb_string_t* pem) {
    config_t& settings = configuration(config);
    // mbedTLS reads PEM with the NUL after it.
    const std::string text(view_of(pem));
    if (mbedtls_pk_parse_key(&settings.key, reinterpret_cast<const unsigned char*>(text.c_str()),
                             text.size() + 1, nullptr, 0) != 0) {
        return -1;
    }

    if (mbedtls_pk_check_pair(&settings.chain.pk, &settings.key) != 0) {
        return -2;
    }

    settings.has_key = true;
    return 0;
}

void fb_tls_config_offer(void* config, const fb_string_t* protocol) {
    configuration(config).protocols.emplace_back(view_of(protocol));
}

void* fb_tls_new(void* config, const fb_string_t* host) {
    config_t& settings = configuration(config);
    std::call_once(settings.set_up, [&] { settings.setup = set_up(settings); });
    auto* tls = new tls_t();
    tls->config = &settings;
    retain(&settings);
    tls->host = view_of(host);
    mbedtls_ssl_init(&tls->ssl);
    tls->setup = settings.setup;
    if (tls->setup == 0) {
        tls->setup = mbedtls_ssl_setup(&tls->ssl, &settings.ssl);
    }

    if (tls->setup == 0 && !settings.server) {
        tls->setup = mbedtls_ssl_set_hostname(&tls->ssl, tls->host.c_str());
    }

    mbedtls_ssl_set_bio(&tls->ssl, tls, send, receive, nullptr);
    return tls;
}

void fb_tls_free(void* tls) {
    tls_t* connection = static_cast<tls_t*>(tls);
    mbedtls_ssl_free(&connection->ssl);
    release(connection->config);
    delete connection;
}

fb_string_t* fb_tls_message(void* tls) {
    return string_of(connection(tls).message);
}

void fb_tls_feed(void* tls, const std::uint8_t* from, const std::int64_t count) {
    tls_t& state = connection(tls);
    if (count == 0) {
        state.ended = true;
        return;
    }

    state.input.insert(state.input.end(), from, from + count);
}

std::int64_t fb_tls_pending(void* tls) {
    const tls_t& state = connection(tls);
    return static_cast<std::int64_t>(state.output.size() - state.taken);
}

std::int64_t fb_tls_take(void* tls, std::uint8_t* into, const std::int64_t capacity) {
    tls_t& state = connection(tls);
    const std::size_t count =
        std::min(state.output.size() - state.taken, static_cast<std::size_t>(capacity));
    std::memcpy(into, state.output.data() + state.taken, count);
    state.taken += count;
    if (state.taken == state.output.size()) {
        state.output.clear();
        state.taken = 0;
    }

    return static_cast<std::int64_t>(count);
}

std::int32_t fb_tls_handshake(void* tls) {
    tls_t& state = connection(tls);
    if (state.setup != 0) {
        state.message = describe(state, state.setup);
        return handshake_failed;
    }

    const int result = mbedtls_ssl_handshake(&state.ssl);
    if (result == 0) {
        state.handshaken = true;
        return handshake_done;
    }

    if (result == MBEDTLS_ERR_SSL_WANT_READ) {
        return handshake_wants_input;
    }

    if (result == MBEDTLS_ERR_SSL_CONN_EOF) {
        state.message = "the stream ended before the TLS handshake did";
        return handshake_ended;
    }

    state.message = describe(state, result);
    return handshake_failed;
}

std::int64_t fb_tls_read(void* tls, std::uint8_t* into, const std::int64_t capacity) {
    tls_t& state = connection(tls);
    if (state.closed) {
        return 0;
    }

    // mbedTLS gives at most a record at a time, which an int counts.
    const auto most =
        static_cast<std::size_t>(std::min<std::int64_t>(capacity, std::numeric_limits<int>::max()));
    while (true) {
        const int result = mbedtls_ssl_read(&state.ssl, into, most);
        if (result > 0) {
            return result;
        }

        switch (result) {
        case MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY:
            state.closed = true;
            return 0;
        case 0:
        case MBEDTLS_ERR_SSL_CONN_EOF:
            state.message = "the stream ended without TLS's close_notify, so what came may be cut "
                            "short";
            return read_cut_short;
        case MBEDTLS_ERR_SSL_WANT_READ:
            return read_wants_input;
        // A TLS 1.3 server's tickets, which this client doesn't keep, come between records.
        case MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET:
            continue;
        default:
            state.message = describe(state, result);
            return read_failed;
        }
    }
}

std::int32_t fb_tls_write(void* tls, const std::uint8_t* from, const std::int64_t count) {
    tls_t& state = connection(tls);
    std::size_t done = 0;
    const auto size = static_cast<std::size_t>(count);
    while (done < size) {
        const int result = mbedtls_ssl_write(&state.ssl, from + done, size - done);
        if (result < 0) {
            state.message = describe(state, result);
            return -1;
        }

        done += static_cast<std::size_t>(result);
    }

    return 0;
}

void fb_tls_close(void* tls) {
    (void)mbedtls_ssl_close_notify(&connection(tls).ssl);
}

fb_string_t* fb_tls_protocol(void* tls) {
    const char* protocol = mbedtls_ssl_get_alpn_protocol(&connection(tls).ssl);
    return protocol != nullptr ? fb_string_from_cstring(protocol) : nullptr;
}

fb_string_t* fb_tls_version(void* tls) {
    return string_of(mbedtls_ssl_get_version_number(&connection(tls).ssl) ==
                             MBEDTLS_SSL_VERSION_TLS1_3
                         ? "TLS 1.3"
                         : "TLS 1.2");
}

fb_string_t* fb_tls_peer_certificate(void* tls) {
    const mbedtls_x509_crt* peer = mbedtls_ssl_get_peer_cert(&connection(tls).ssl);
    if (peer == nullptr) {
        return nullptr;
    }

    std::string pem;
    fb::runtime::append_pem(pem, std::span(peer->raw.p, peer->raw.len));
    return string_of(pem);
}
}
