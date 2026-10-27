// The ACME tests' certificate authority: RFC 8555's directory, nonces, accounts, orders,
// authorizations, HTTP-01 challenges, finalizing, and certificates, over mbedTLS.

#include "acme_ca.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <mbedtls/asn1.h>
#include <mbedtls/oid.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/x509_csr.h>
#include <optional>
#include <psa/crypto.h>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

/// The root's key and certificate, which tests/native/acme_ca made with OpenSSL, valid from 2020
/// to 2120.
constexpr std::string_view root_key = R"(-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgh1l+cMXItDQ2OyJs
lYy7ki/fW6fmWQ3mn/jmOk+3/uahRANCAATAARwJUVq7FFDB1Gxvzn2+sHR67tcK
sOLKclGKSbpHoZrtFixoBSLsXwTqqifh8yKeRkmjj99nnlicqCj9uuaT
-----END PRIVATE KEY-----
)";

constexpr std::string_view root_certificate = R"(-----BEGIN CERTIFICATE-----
MIIBrTCCAVOgAwIBAgIUClgB3BMRqyBaZlk0fdr0DZxIaTwwCgYIKoZIzj0EAwIw
IzEhMB8GA1UEAwwYRnV0dXJlQmxpdHogVGVzdCBBQ01FIENBMCAXDTIwMDEwMTAw
MDAwMFoYDzIxMjAwMTAxMDAwMDAwWjAjMSEwHwYDVQQDDBhGdXR1cmVCbGl0eiBU
ZXN0IEFDTUUgQ0EwWTATBgcqhkjOPQIBBggqhkjOPQMBBwNCAATAARwJUVq7FFDB
1Gxvzn2+sHR67tcKsOLKclGKSbpHoZrtFixoBSLsXwTqqifh8yKeRkmjj99nnlic
qCj9uuaTo2MwYTAdBgNVHQ4EFgQU2AmJuPn/hI9D1oUXKC+5d3HOAAowHwYDVR0j
BBgwFoAU2AmJuPn/hI9D1oUXKC+5d3HOAAowDwYDVR0TAQH/BAUwAwEB/zAOBgNV
HQ8BAf8EBAMCAQYwCgYIKoZIzj0EAwIDSAAwRQIhAMCkoF/f7A2+OgwntPslxAP+
iQbE4g5btQlv1IOKLYEDAiBYA0pX2Cl2Z96wtVmzv9wU0wzQETfg72d/EYQSQfXh
mw==
-----END CERTIFICATE-----
)";

constexpr std::string_view root_name = "CN=FutureBlitz Test ACME CA";
constexpr std::string_view error_prefix = "urn:ietf:params:acme:error:";
/// When everything it makes expires, as RFC 8555 has each object say.
constexpr std::string_view expires = "2120-01-01T00:00:00Z";

// JSON, as much of it as ACME's objects need.

struct json_t {
    enum class kind_t : std::uint8_t { NUL, BOOL, NUMBER, STRING, ARRAY, OBJECT };

    kind_t kind = kind_t::NUL;
    bool boolean = false;
    std::string text;
    std::vector<json_t> items;
    std::vector<std::pair<std::string, json_t>> members;

    /// The member named `key` of an object, or null.
    [[nodiscard]] const json_t* get(const std::string_view key) const {
        const auto found = std::ranges::find(members, key, &std::pair<std::string, json_t>::first);
        return found != members.end() ? &found->second : nullptr;
    }

    /// The text of a string member, or nothing.
    [[nodiscard]] std::optional<std::string> string(const std::string_view key) const {
        const json_t* member = get(key);
        return member != nullptr && member->kind == kind_t::STRING ? std::optional(member->text)
                                                                   : std::nullopt;
    }
};

class json_reader_t {
public:
    explicit json_reader_t(const std::string_view text) : m_text(text) {}

    /// The one value the text is, or nothing for what isn't JSON.
    std::optional<json_t> read() {
        std::optional<json_t> value = next();
        skip();
        return m_at == m_text.size() ? value : std::nullopt;
    }

private:
    std::string_view m_text;
    std::size_t m_at = 0;
    int m_depth = 0;

    void skip() {
        while (m_at < m_text.size() && (m_text[m_at] == ' ' || m_text[m_at] == '\t' ||
                                        m_text[m_at] == '\n' || m_text[m_at] == '\r')) {
            ++m_at;
        }
    }

    bool accept(const char c) {
        skip();
        if (m_at < m_text.size() && m_text[m_at] == c) {
            ++m_at;
            return true;
        }

        return false;
    }

    bool word(const std::string_view expected) {
        if (m_text.substr(m_at, expected.size()) != expected) {
            return false;
        }

        m_at += expected.size();
        return true;
    }

    std::optional<json_t> next() {
        // ACME's objects nest a few deep; a test's never come near this.
        constexpr int deepest = 32;
        skip();
        if (m_at >= m_text.size() || m_depth > deepest) {
            return std::nullopt;
        }

        json_t value;
        const char c = m_text[m_at];
        if (c == '{' || c == '[') {
            ++m_at;
            ++m_depth;
            const bool object = c == '{';
            value.kind = object ? json_t::kind_t::OBJECT : json_t::kind_t::ARRAY;
            if (!accept(object ? '}' : ']')) {
                do {
                    std::string key;
                    if (object) {
                        skip();
                        std::optional<std::string> name = string();
                        if (!name || !accept(':')) {
                            return std::nullopt;
                        }

                        key = std::move(*name);
                    }

                    std::optional<json_t> item = next();
                    if (!item) {
                        return std::nullopt;
                    }

                    if (object) {
                        value.members.emplace_back(std::move(key), std::move(*item));
                    } else {
                        value.items.push_back(std::move(*item));
                    }
                } while (accept(','));
                if (!accept(object ? '}' : ']')) {
                    return std::nullopt;
                }
            }

            --m_depth;
            return value;
        }

        if (c == '"') {
            std::optional<std::string> text = string();
            if (!text) {
                return std::nullopt;
            }

            value.kind = json_t::kind_t::STRING;
            value.text = std::move(*text);
            return value;
        }

        if (word("true") || word("false")) {
            value.kind = json_t::kind_t::BOOL;
            value.boolean = c == 't';
            return value;
        }

        if (word("null")) {
            return value;
        }

        const std::size_t begin = m_at;
        while (m_at < m_text.size() && std::string_view("+-.0123456789eE").contains(m_text[m_at])) {
            ++m_at;
        }

        if (m_at == begin) {
            return std::nullopt;
        }

        value.kind = json_t::kind_t::NUMBER;
        value.text = std::string(m_text.substr(begin, m_at - begin));
        return value;
    }

    /// A string from its opening quote, its escapes undone, as UTF-8.
    std::optional<std::string> string() {
        if (m_at >= m_text.size() || m_text[m_at] != '"') {
            return std::nullopt;
        }

        ++m_at;
        std::string text;
        while (m_at < m_text.size() && m_text[m_at] != '"') {
            const char c = m_text[m_at++];
            if (c != '\\') {
                text += c;
                continue;
            }

            if (m_at >= m_text.size()) {
                return std::nullopt;
            }

            const char escaped = m_text[m_at++];
            switch (escaped) {
            case 'n':
                text += '\n';
                break;
            case 't':
                text += '\t';
                break;
            case 'r':
                text += '\r';
                break;
            case 'b':
                text += '\b';
                break;
            case 'f':
                text += '\f';
                break;
            case 'u': {
                unsigned point = 0;
                const std::string_view digits = m_text.substr(m_at, 4);
                if (digits.size() < 4 ||
                    std::from_chars(digits.data(), digits.data() + 4, point, 16).ptr !=
                        digits.data() + 4) {
                    return std::nullopt;
                }

                m_at += 4;
                // ACME's objects are ASCII, as a test's are; a code point past it is kept as '?'.
                text += point < 0x80 ? static_cast<char>(point) : '?';
                break;
            }
            default:
                text += escaped;
                break;
            }
        }

        if (m_at >= m_text.size()) {
            return std::nullopt;
        }

        ++m_at;
        return text;
    }
};

/// `text` as a JSON string, in quotes, with its quotes, backslashes, and control characters
/// escaped.
std::string quoted(const std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            out += std::format("\\u{:04x}", static_cast<unsigned>(c));
        } else {
            out += c;
        }
    }

    return out + "\"";
}

/// Strings as a JSON array of strings.
std::string string_array(const std::vector<std::string>& texts) {
    std::string out = "[";
    for (std::size_t i = 0; i < texts.size(); ++i) {
        out += (i == 0 ? "" : ",") + quoted(texts[i]);
    }

    return out + "]";
}

// Base64url, without padding, as JOSE has it.

constexpr std::string_view base64url_digits =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::string base64url(const std::span<const std::uint8_t> bytes) {
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

        text += base64url_digits[(group >> 18U) & 63U];
        text += base64url_digits[(group >> 12U) & 63U];
        if (left > 1) {
            text += base64url_digits[(group >> 6U) & 63U];
        }

        if (left > 2) {
            text += base64url_digits[group & 63U];
        }
    }

    return text;
}

std::optional<std::vector<std::uint8_t>> from_base64url(const std::string_view text) {
    std::vector<std::uint8_t> bytes;
    std::uint32_t group = 0;
    unsigned bits = 0;
    for (const char c : text) {
        const std::size_t value = base64url_digits.find(c);
        if (value == std::string_view::npos) {
            return std::nullopt;
        }

        group = (group << 6U) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            bytes.push_back(static_cast<std::uint8_t>((group >> bits) & 0xFFU));
        }
    }

    // What's left past the last byte is padding, and is zero.
    if (bits >= 6 || (group & ((1U << bits) - 1U)) != 0) {
        return std::nullopt;
    }

    return bytes;
}

std::span<const std::uint8_t> bytes_of(const std::string_view text) {
    return { reinterpret_cast<const std::uint8_t*>(text.data()), text.size() };
}

std::string text_of(const std::span<const std::uint8_t> bytes) {
    return { reinterpret_cast<const char*>(bytes.data()), bytes.size() };
}

std::array<std::uint8_t, 32> sha256(const std::span<const std::uint8_t> bytes) {
    std::array<std::uint8_t, 32> hash{};
    std::size_t length = 0;
    (void)psa_hash_compute(PSA_ALG_SHA_256, bytes.data(), bytes.size(), hash.data(), hash.size(),
                           &length);
    return hash;
}

/// A random base64url string of `count` bytes, as a nonce or a token is.
std::string random_text(const std::size_t count) {
    std::vector<std::uint8_t> bytes(count);
    (void)psa_generate_random(bytes.data(), bytes.size());
    return base64url(bytes);
}

/// The most names an order has, as Let's Encrypt has, and the most bytes a certificate's DER
/// takes.
constexpr std::size_t most_names = 100;
constexpr std::size_t most_certificate = 65536;

// The CA's records.

struct account_t {
    /// Its key's point, its x and y in base64url, as its JWK has them.
    std::string x;
    std::string y;
    std::string thumbprint;
    std::vector<std::string> contact;
};

struct authorization_t {
    std::size_t account = 0;
    std::string name;
    std::string token;
    std::string status = "pending";
    std::string challenge_status = "pending";
    std::string error;
    /// How many times it's been asked about since its challenge was, which its check waits for.
    std::size_t looks = 0;
};

struct order_t {
    std::size_t account = 0;
    std::vector<std::string> names;
    std::vector<std::size_t> authorizations;
    std::string status = "pending";
    std::string certificate;
};

/// A request's JWS, checked: its account, or its key for a new one, and its payload.
struct signed_t {
    std::optional<std::size_t> account;
    std::string x;
    std::string y;
    std::string payload;
};

/// `x` and `y`'s JWK thumbprint, RFC 7638's: SHA-256 of the JWK's members in order, in base64url.
std::string thumbprint(const std::string_view x, const std::string_view y) {
    const std::string jwk = std::format(R"({{"crv":"P-256","kty":"EC","x":"{}","y":"{}"}})", x, y);
    return base64url(sha256(bytes_of(jwk)));
}

/// Whether `signature`, r then s, is the P-256 key's of `x` and `y` over `input`.
bool verify_es256(const std::string_view x, const std::string_view y, const std::string_view input,
                  const std::span<const std::uint8_t> signature) {
    const std::optional<std::vector<std::uint8_t>> x_bytes = from_base64url(x);
    const std::optional<std::vector<std::uint8_t>> y_bytes = from_base64url(y);
    if (!x_bytes || !y_bytes || x_bytes->size() != 32 || y_bytes->size() != 32 ||
        signature.size() != 64) {
        return false;
    }

    std::vector<std::uint8_t> point{ 0x04 };
    point.insert(point.end(), x_bytes->begin(), x_bytes->end());
    point.insert(point.end(), y_bytes->begin(), y_bytes->end());
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_VERIFY_MESSAGE);
    psa_set_key_algorithm(&attributes, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    mbedtls_svc_key_id_t key{};
    if (psa_import_key(&attributes, point.data(), point.size(), &key) != PSA_SUCCESS) {
        return false;
    }

    const bool good =
        psa_verify_message(key, PSA_ALG_ECDSA(PSA_ALG_SHA_256), bytes_of(input).data(),
                           input.size(), signature.data(), signature.size()) == PSA_SUCCESS;
    (void)psa_destroy_key(key);
    return good;
}

/// A CSR's signature, the BIT STRING after its request info and signature algorithm, which
/// mbedTLS keeps to itself.
std::optional<std::span<const std::uint8_t>> signature_of(const mbedtls_x509_csr& csr) {
    unsigned char* at = csr.cri.p + csr.cri.len;
    const unsigned char* end = csr.raw.p + csr.raw.len;
    std::size_t length = 0;
    if (mbedtls_asn1_get_tag(&at, end, &length, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) !=
        0) {
        return std::nullopt;
    }

    at += length;
    if (mbedtls_asn1_get_tag(&at, end, &length, MBEDTLS_ASN1_BIT_STRING) != 0 || length < 1 ||
        *at != 0) {
        return std::nullopt;
    }

    return std::span<const std::uint8_t>(at + 1, length - 1);
}

/// A certificate's PEM, from DER.
std::string pem_of(const std::span<const std::uint8_t> der) {
    static constexpr std::string_view digits =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string body;
    for (std::size_t at = 0; at < der.size(); at += 3) {
        const std::size_t left = der.size() - at;
        std::uint32_t group = static_cast<std::uint32_t>(der[at]) << 16U;
        if (left > 1) {
            group |= static_cast<std::uint32_t>(der[at + 1]) << 8U;
        }

        if (left > 2) {
            group |= der[at + 2];
        }

        body += digits[(group >> 18U) & 63U];
        body += digits[(group >> 12U) & 63U];
        body += left > 1 ? digits[(group >> 6U) & 63U] : '=';
        body += left > 2 ? digits[group & 63U] : '=';
    }

    std::string pem = "-----BEGIN CERTIFICATE-----\n";
    for (std::size_t at = 0; at < body.size(); at += 64) {
        pem += body.substr(at, 64) + "\n";
    }

    return pem + "-----END CERTIFICATE-----\n";
}

} // namespace

namespace fb::test {

struct acme_ca_t::state_t {
    std::vector<std::string> nonces;
    std::size_t next_nonce = 0;
    std::vector<account_t> accounts;
    std::vector<order_t> orders;
    std::vector<authorization_t> authorizations;
    /// How many requests' nonces it refuses next, whatever they are.
    std::size_t refusing = 0;
    mbedtls_pk_context key{};
};

namespace {

/// A problem document, RFC 7807's, of ACME's error `type`.
acme_response_t problem(const int status, const std::string_view type,
                        const std::string_view detail) {
    return {
        .status = status,
        .headers = { { "Content-Type", "application/problem+json" } },
        .body = std::format(R"({{"type":{},"detail":{},"status":{}}})",
                            quoted(std::string(error_prefix) + std::string(type)), quoted(detail),
                            status),
    };
}

acme_response_t json_response(const int status, std::string body) {
    return { .status = status,
             .headers = { { "Content-Type", "application/json" } },
             .body = std::move(body) };
}

/// The number at the end of `path` after `prefix`, from 1, as an index into what it names, or
/// nothing.
std::optional<std::size_t> index_after(const std::string_view path, const std::string_view prefix,
                                       const std::size_t count) {
    if (!path.starts_with(prefix)) {
        return std::nullopt;
    }

    std::size_t number = 0;
    const std::string_view digits = path.substr(prefix.size());
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), number);
    if (error != std::errc{} || end != digits.data() + digits.size() || number == 0 ||
        number > count) {
        return std::nullopt;
    }

    return number - 1;
}

} // namespace

std::optional<std::string> body_of_ok(const std::string_view response) {
    const std::size_t end = response.find("\r\n\r\n");
    // "HTTP/1.0 200" or "HTTP/1.1 200", and a reason.
    if (end == std::string_view::npos || !response.starts_with("HTTP/1.") || response.size() < 12 ||
        response.substr(8, 4) != " 200") {
        return std::nullopt;
    }

    std::optional<std::size_t> length;
    bool chunked = false;
    for (std::size_t at = response.find("\r\n") + 2; at < end;) {
        const std::size_t next = response.find("\r\n", at);
        const std::string_view line = response.substr(at, next - at);
        at = next + 2;
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos) {
            continue;
        }

        std::string name(line.substr(0, colon));
        std::ranges::transform(name, name.begin(), [](const char c) {
            return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
        });
        std::string_view value = line.substr(colon + 1);
        while (value.starts_with(' ')) {
            value.remove_prefix(1);
        }

        if (name == "content-length") {
            std::size_t count = 0;
            const auto [stop, error] =
                std::from_chars(value.data(), value.data() + value.size(), count);
            if (error != std::errc{} || stop != value.data() + value.size()) {
                return std::nullopt;
            }

            length = count;
        } else if (name == "transfer-encoding") {
            chunked = value.contains("chunked");
        }
    }

    std::string_view rest = response.substr(end + 4);
    if (chunked) {
        // Each chunk's size in hex, maybe with extensions after a ';', its bytes, and CRLF, to
        // one of 0.
        std::string body;
        while (true) {
            const std::size_t line_end = rest.find("\r\n");
            if (line_end == std::string_view::npos) {
                return std::nullopt;
            }

            const std::string_view size_text = rest.substr(0, std::min(line_end, rest.find(';')));
            std::size_t size = 0;
            const auto [stop, error] =
                std::from_chars(size_text.data(), size_text.data() + size_text.size(), size, 16);
            if (error != std::errc{} || stop == size_text.data()) {
                return std::nullopt;
            }

            rest.remove_prefix(line_end + 2);
            if (size == 0) {
                return body;
            }

            if (rest.size() < size + 2 || rest.substr(size, 2) != "\r\n") {
                return std::nullopt;
            }

            body += rest.substr(0, size);
            rest.remove_prefix(size + 2);
        }
    }

    if (length) {
        return rest.size() >= *length ? std::optional(std::string(rest.substr(0, *length)))
                                      : std::nullopt;
    }

    return std::string(rest);
}

acme_ca_t::acme_ca_t(std::string base, acme_fetch_t fetch)
    : m_base(std::move(base)), m_fetch(std::move(fetch)), m_state(std::make_unique<state_t>()) {
    (void)psa_crypto_init();
    mbedtls_pk_init(&m_state->key);
    const std::string key(root_key);
    (void)mbedtls_pk_parse_key(&m_state->key, bytes_of(key).data(), key.size() + 1, nullptr, 0);
}

acme_ca_t::~acme_ca_t() {
    mbedtls_pk_free(&m_state->key);
}

void acme_ca_t::refuse_nonce() {
    ++m_state->refusing;
}

const std::string& acme_ca_t::root_pem() {
    static const std::string pem(root_certificate);
    return pem;
}

acme_response_t acme_ca_t::handle(const acme_request_t& request) {
    state_t& state = *m_state;
    const std::string& base = m_base;
    const auto new_nonce = [&] {
        state.nonces.push_back(std::format("{}{}", random_text(12), state.next_nonce++));
        return state.nonces.back();
    };
    // Every answer to a POST has a nonce for the next.
    const auto with_nonce = [&](acme_response_t response) {
        response.headers.emplace_back("Replay-Nonce", new_nonce());
        return response;
    };
    const auto account_url = [&](const std::size_t index) {
        return std::format("{}/account/{}", base, index + 1);
    };
    // An authorization's one challenge, and the authorization with it.
    const auto challenge_json = [&](const std::size_t index) {
        const authorization_t& authorization = state.authorizations[index];
        const std::string error =
            authorization.error.empty()
                ? ""
                : std::format(R"(,"error":{{"type":{},"detail":{}}})",
                              quoted(std::string(error_prefix) + "incorrectResponse"),
                              quoted(authorization.error));
        return std::format(R"({{"type":"http-01","url":{},"token":{},"status":{}{}}})",
                           quoted(std::format("{}/challenge/{}", base, index + 1)),
                           quoted(authorization.token), quoted(authorization.challenge_status),
                           error);
    };
    const auto authorization_json = [&](const std::size_t index) {
        const authorization_t& authorization = state.authorizations[index];
        return std::format(R"({{"status":{},"expires":{},"identifier":{{"type":"dns","value":{}}},)"
                           R"("challenges":[{}]}})",
                           quoted(authorization.status), quoted(expires),
                           quoted(authorization.name), challenge_json(index));
    };
    const auto order_json = [&](const std::size_t index) {
        const order_t& order = state.orders[index];
        std::string identifiers;
        for (const std::string& name : order.names) {
            identifiers += std::format(R"({}{{"type":"dns","value":{}}})",
                                       identifiers.empty() ? "" : ",", quoted(name));
        }

        std::vector<std::string> authorizations;
        for (const std::size_t authorization : order.authorizations) {
            authorizations.push_back(std::format("{}/authz/{}", base, authorization + 1));
        }

        const std::string certificate =
            order.status == "valid"
                ? std::format(R"(,"certificate":{})",
                              quoted(std::format("{}/certificate/{}", base, index + 1)))
                : "";
        return std::format(
            R"({{"status":{},"expires":{},"identifiers":[{}],"authorizations":{},"finalize":{}{}}})",
            quoted(order.status), quoted(expires), identifiers, string_array(authorizations),
            quoted(std::format("{}/order/{}/finalize", base, index + 1)), certificate);
    };

    if (request.path == "/dir" && request.method == "GET") {
        return json_response(200, std::format(R"({{"newNonce":{},"newAccount":{},"newOrder":{},)"
                                              R"("meta":{{"termsOfService":{}}}}})",
                                              quoted(base + "/nonce"), quoted(base + "/account"),
                                              quoted(base + "/order"), quoted(base + "/terms")));
    }

    if (request.path == "/nonce" && (request.method == "HEAD" || request.method == "GET")) {
        return with_nonce({ .status = request.method == "HEAD" ? 200 : 204,
                            .headers = { { "Cache-Control", "no-store" } } });
    }

    if (request.method != "POST") {
        return problem(405, "malformed", "the CA takes a POST here");
    }

    if (request.content_type != "application/jose+json") {
        return with_nonce(
            problem(415, "malformed", "a POST's Content-Type is application/jose+json"));
    }

    // A flattened JWS: its protected header, which names the request's URL, a nonce of the CA's,
    // and its key or account, its payload, and ES256's signature over both.
    const std::optional<json_t> jws = json_reader_t(request.body).read();
    const std::optional<std::string> protected_text = jws ? jws->string("protected") : std::nullopt;
    const std::optional<std::string> payload_text = jws ? jws->string("payload") : std::nullopt;
    const std::optional<std::string> signature_text = jws ? jws->string("signature") : std::nullopt;
    if (!protected_text || !payload_text || !signature_text) {
        return with_nonce(problem(400, "malformed", "the body isn't a flattened JWS"));
    }

    const std::optional<std::vector<std::uint8_t>> header_bytes = from_base64url(*protected_text);
    const std::optional<std::vector<std::uint8_t>> payload_bytes = from_base64url(*payload_text);
    const std::optional<std::vector<std::uint8_t>> signature = from_base64url(*signature_text);
    const std::optional<json_t> header =
        header_bytes ? json_reader_t(text_of(*header_bytes)).read() : std::nullopt;
    if (!payload_bytes || !signature || !header) {
        return with_nonce(problem(400, "malformed", "the JWS's parts aren't base64url of JSON"));
    }

    if (header->string("alg") != "ES256") {
        return with_nonce(problem(400, "badSignatureAlgorithm", "the CA takes ES256"));
    }

    const std::optional<std::string> nonce = header->string("nonce");
    const auto used = nonce ? std::ranges::find(state.nonces, *nonce) : state.nonces.end();
    if (used == state.nonces.end()) {
        return with_nonce(problem(400, "badNonce", "the nonce isn't one the CA gave"));
    }

    state.nonces.erase(used);
    if (state.refusing > 0) {
        --state.refusing;
        return with_nonce(problem(400, "badNonce", "the CA refused the nonce"));
    }

    if (header->string("url") != base + request.path) {
        return with_nonce(problem(401, "unauthorized", "the JWS's url isn't the request's"));
    }

    const json_t* jwk = header->get("jwk");
    const std::optional<std::string> kid = header->string("kid");
    signed_t request_signed{ .payload = text_of(*payload_bytes) };
    if ((jwk != nullptr) == kid.has_value()) {
        return with_nonce(problem(400, "malformed", "a JWS names a jwk or a kid, one of them"));
    }

    if (jwk != nullptr) {
        if (jwk->string("kty") != "EC" || jwk->string("crv") != "P-256" || !jwk->string("x") ||
            !jwk->string("y")) {
            return with_nonce(problem(400, "badPublicKey", "the CA takes P-256 keys"));
        }

        request_signed.x = *jwk->string("x");
        request_signed.y = *jwk->string("y");
    } else {
        request_signed.account = index_after(*kid, base + "/account/", state.accounts.size());
        if (!request_signed.account) {
            return with_nonce(problem(400, "accountDoesNotExist", "the kid isn't an account"));
        }

        request_signed.x = state.accounts[*request_signed.account].x;
        request_signed.y = state.accounts[*request_signed.account].y;
    }

    if (!verify_es256(request_signed.x, request_signed.y,
                      std::format("{}.{}", *protected_text, *payload_text), *signature)) {
        return with_nonce(problem(403, "unauthorized", "the JWS's signature isn't its key's"));
    }

    // A POST-as-GET's payload is empty, and a new account's or order's is an object.
    const std::optional<json_t> payload = request_signed.payload.empty()
                                              ? std::optional<json_t>(json_t{})
                                              : json_reader_t(request_signed.payload).read();
    if (!payload) {
        return with_nonce(problem(400, "malformed", "the payload isn't JSON"));
    }

    if (request.path == "/account") {
        if (jwk == nullptr) {
            return with_nonce(problem(400, "malformed", "a new account's JWS has its jwk"));
        }

        const auto known = std::ranges::find_if(state.accounts, [&](const account_t& account) {
            return account.x == request_signed.x && account.y == request_signed.y;
        });
        const json_t* only_existing = payload->get("onlyReturnExisting");
        bool made = false;
        if (known == state.accounts.end()) {
            if (only_existing != nullptr && only_existing->boolean) {
                return with_nonce(
                    problem(400, "accountDoesNotExist", "the key has no account here"));
            }

            const json_t* agreed = payload->get("termsOfServiceAgreed");
            if (agreed == nullptr || !agreed->boolean) {
                return with_nonce(
                    problem(403, "userActionRequired", "a new account agrees to the terms"));
            }

            account_t account{ .x = request_signed.x,
                               .y = request_signed.y,
                               .thumbprint = thumbprint(request_signed.x, request_signed.y) };
            if (const json_t* contact = payload->get("contact")) {
                for (const json_t& item : contact->items) {
                    if (item.kind != json_t::kind_t::STRING || !item.text.starts_with("mailto:")) {
                        return with_nonce(
                            problem(400, "unsupportedContact", "a contact is a mailto: URL"));
                    }

                    account.contact.push_back(item.text);
                }
            }

            state.accounts.push_back(std::move(account));
            made = true;
        }

        const auto index = static_cast<std::size_t>(
            std::distance(state.accounts.begin(), made ? state.accounts.end() - 1 : known));
        acme_response_t response = json_response(
            made ? 201 : 200, std::format(R"({{"status":"valid","contact":{},"orders":{}}})",
                                          string_array(state.accounts[index].contact),
                                          quoted(account_url(index) + "/orders")));
        response.headers.emplace_back("Location", account_url(index));
        return with_nonce(std::move(response));
    }

    // Everything else is an account's.
    if (!request_signed.account) {
        return with_nonce(problem(400, "malformed", "the JWS names its account by kid"));
    }

    const std::size_t account = *request_signed.account;
    if (request.path == "/order") {
        const json_t* identifiers = payload->get("identifiers");
        if (identifiers == nullptr || identifiers->items.empty()) {
            return with_nonce(problem(400, "malformed", "an order has identifiers"));
        }

        if (identifiers->items.size() > most_names) {
            return with_nonce(problem(400, "rejectedIdentifier",
                                      std::format("an order has {} names at most", most_names)));
        }

        order_t order{ .account = account };
        for (const json_t& identifier : identifiers->items) {
            if (identifier.string("type") != "dns" || !identifier.string("value")) {
                return with_nonce(
                    problem(400, "unsupportedIdentifier", "the CA takes dns identifiers"));
            }

            order.names.push_back(*identifier.string("value"));
            order.authorizations.push_back(state.authorizations.size());
            state.authorizations.push_back(
                { .account = account, .name = order.names.back(), .token = random_text(32) });
        }

        state.orders.push_back(std::move(order));
        const std::size_t index = state.orders.size() - 1;
        acme_response_t response = json_response(201, order_json(index));
        response.headers.emplace_back("Location", std::format("{}/order/{}", base, index + 1));
        return with_nonce(std::move(response));
    }

    if (const std::optional<std::size_t> index =
            index_after(request.path, "/authz/", state.authorizations.size())) {
        authorization_t& authorization = state.authorizations[*index];
        if (authorization.account != account) {
            return with_nonce(
                problem(403, "unauthorized", "the authorization isn't the account's"));
        }

        // A challenge being checked is checked the second time its authorization is asked about,
        // so a client waits, as Retry-After says, at least once: the key authorization, the token
        // and the account key's thumbprint, at the name's well-known path.
        acme_response_t response = json_response(200, "");
        if (authorization.challenge_status == "processing" && ++authorization.looks > 1) {
            const std::string expected =
                std::format("{}.{}", authorization.token, state.accounts[account].thumbprint);
            // RFC 8555 8.3: whitespace after the key authorization is the server's to send.
            std::optional<std::string> fetched = m_fetch(authorization.name, authorization.token);
            while (fetched && !fetched->empty() &&
                   std::string_view(" \t\r\n").contains(fetched->back())) {
                fetched->pop_back();
            }

            const bool good = fetched && *fetched == expected;
            authorization.status = good ? "valid" : "invalid";
            authorization.challenge_status = authorization.status;
            if (!good) {
                authorization.error =
                    fetched ? std::format("the response at {} isn't the key authorization",
                                          authorization.name)
                            : std::format("{} didn't answer the challenge", authorization.name);
            }

            for (order_t& order : state.orders) {
                if (std::ranges::contains(order.authorizations, *index) &&
                    order.status == "pending") {
                    const bool all = std::ranges::all_of(order.authorizations, [&](const auto i) {
                        return state.authorizations[i].status == "valid";
                    });
                    order.status = good ? (all ? "ready" : "pending") : "invalid";
                }
            }
        } else if (authorization.challenge_status == "processing") {
            response.headers.emplace_back("Retry-After", "1");
        }

        response.body = authorization_json(*index);
        return with_nonce(std::move(response));
    }

    if (const std::optional<std::size_t> index =
            index_after(request.path, "/challenge/", state.authorizations.size())) {
        authorization_t& authorization = state.authorizations[*index];
        if (authorization.account != account) {
            return with_nonce(problem(403, "unauthorized", "the challenge isn't the account's"));
        }

        // A POST of {} has the CA check it, later, as Pebble does: see the authorization's. A
        // POST-as-GET only asks about it.
        if (authorization.challenge_status == "pending" && !request_signed.payload.empty() &&
            payload->kind == json_t::kind_t::OBJECT) {
            authorization.challenge_status = "processing";
        }

        acme_response_t response = json_response(200, challenge_json(*index));
        response.headers.emplace_back("Link",
                                      std::format("<{}/authz/{}>;rel=\"up\"", base, *index + 1));
        return with_nonce(std::move(response));
    }

    if (request.path.ends_with("/finalize")) {
        const std::string_view path = request.path;
        const std::optional<std::size_t> index =
            index_after(path.substr(0, path.size() - 9), "/order/", state.orders.size());
        if (!index || state.orders[*index].account != account) {
            return with_nonce(problem(404, "malformed", "there's no such order"));
        }

        order_t& order = state.orders[*index];
        if (order.status != "ready") {
            return with_nonce(problem(403, "orderNotReady", "the order isn't ready"));
        }

        const std::optional<std::string> csr_text = payload->string("csr");
        const std::optional<std::vector<std::uint8_t>> csr_der =
            csr_text ? from_base64url(*csr_text) : std::nullopt;
        mbedtls_x509_csr csr;
        mbedtls_x509_csr_init(&csr);
        if (!csr_der || mbedtls_x509_csr_parse_der(&csr, csr_der->data(), csr_der->size()) != 0) {
            mbedtls_x509_csr_free(&csr);
            return with_nonce(problem(400, "badCSR", "the CSR doesn't read"));
        }

        // The CSR's signature is its own key's, and its names are the order's, in any order.
        const std::array<std::uint8_t, 32> hash = sha256({ csr.cri.p, csr.cri.len });
        std::vector<std::string> names;
        for (const mbedtls_x509_sequence* entry = &csr.subject_alt_names;
             entry != nullptr && entry->buf.p != nullptr; entry = entry->next) {
            mbedtls_x509_subject_alternative_name san{};
            if (mbedtls_x509_parse_subject_alt_name(&entry->buf, &san) == 0 &&
                san.type == MBEDTLS_X509_SAN_DNS_NAME) {
                names.push_back(
                    text_of({ san.san.unstructured_name.p, san.san.unstructured_name.len }));
            }

            mbedtls_x509_free_subject_alt_name(&san);
        }

        std::vector<std::string> wanted = order.names;
        std::ranges::sort(names);
        std::ranges::sort(wanted);
        const std::optional<std::span<const std::uint8_t>> csr_signature = signature_of(csr);
        const bool signed_well =
            csr_signature && mbedtls_pk_verify(&csr.pk, MBEDTLS_MD_SHA256, hash.data(), hash.size(),
                                               csr_signature->data(), csr_signature->size()) == 0;
        if (!signed_well || names != wanted) {
            mbedtls_x509_csr_free(&csr);
            return with_nonce(problem(400, "badCSR",
                                      signed_well ? "the CSR's names aren't the order's"
                                                  : "the CSR isn't signed by its key"));
        }

        // The certificate: for the names, from the CSR's key, signed by the root.
        std::vector<mbedtls_x509_san_list> sans(names.size());
        for (std::size_t i = 0; i < names.size(); ++i) {
            sans[i].node.type = MBEDTLS_X509_SAN_DNS_NAME;
            sans[i].node.san.unstructured_name.p =
                reinterpret_cast<unsigned char*>(names[i].data());
            sans[i].node.san.unstructured_name.len = names[i].size();
            sans[i].next = i + 1 < names.size() ? &sans[i + 1] : nullptr;
        }

        mbedtls_x509write_cert writer;
        mbedtls_x509write_crt_init(&writer);
        mbedtls_x509write_crt_set_version(&writer, MBEDTLS_X509_CRT_VERSION_3);
        // 0x10, then the order's number in 8 bytes, big-endian, so no two are the same.
        std::array<std::uint8_t, 9> serial{ 0x10 };
        for (std::size_t i = 0; i < 8; ++i) {
            serial[8 - i] = static_cast<std::uint8_t>((*index + 1) >> (8 * i));
        }

        (void)mbedtls_x509write_crt_set_serial_raw(&writer, serial.data(), serial.size());
        (void)mbedtls_x509write_crt_set_validity(&writer, "20200101000000", "21200101000000");
        (void)mbedtls_x509write_crt_set_issuer_name(&writer, std::string(root_name).c_str());
        (void)mbedtls_x509write_crt_set_subject_name(&writer,
                                                     ("CN=" + order.names.front()).c_str());
        mbedtls_x509write_crt_set_subject_key(&writer, &csr.pk);
        mbedtls_x509write_crt_set_issuer_key(&writer, &state.key);
        mbedtls_x509write_crt_set_md_alg(&writer, MBEDTLS_MD_SHA256);
        (void)mbedtls_x509write_crt_set_basic_constraints(&writer, 0, -1);
        (void)mbedtls_x509write_crt_set_key_usage(&writer, MBEDTLS_X509_KU_DIGITAL_SIGNATURE);
        (void)mbedtls_x509write_crt_set_subject_alternative_name(&writer, sans.data());
        // A buffer twice as big again for a certificate of many names.
        std::vector<std::uint8_t> der(4096);
        int written = mbedtls_x509write_crt_der(&writer, der.data(), der.size());
        while (written == MBEDTLS_ERR_ASN1_BUF_TOO_SMALL && der.size() < most_certificate) {
            der.resize(der.size() * 2);
            written = mbedtls_x509write_crt_der(&writer, der.data(), der.size());
        }

        mbedtls_x509write_crt_free(&writer);
        mbedtls_x509_csr_free(&csr);
        if (written <= 0) {
            return with_nonce(problem(500, "serverInternal", "the certificate couldn't be made"));
        }

        // The DER is at the end of the buffer.
        order.certificate = pem_of({ der.data() + der.size() - static_cast<std::size_t>(written),
                                     static_cast<std::size_t>(written) }) +
                            root_pem();
        order.status = "valid";
        acme_response_t response = json_response(200, order_json(*index));
        response.headers.emplace_back("Location", std::format("{}/order/{}", base, *index + 1));
        return with_nonce(std::move(response));
    }

    if (const std::optional<std::size_t> index =
            index_after(request.path, "/order/", state.orders.size())) {
        if (state.orders[*index].account != account) {
            return with_nonce(problem(403, "unauthorized", "the order isn't the account's"));
        }

        return with_nonce(json_response(200, order_json(*index)));
    }

    if (const std::optional<std::size_t> index =
            index_after(request.path, "/certificate/", state.orders.size())) {
        const order_t& order = state.orders[*index];
        if (order.account != account || order.status != "valid") {
            return with_nonce(problem(404, "malformed", "there's no such certificate"));
        }

        return with_nonce({ .status = 200,
                            .headers = { { "Content-Type", "application/pem-certificate-chain" } },
                            .body = order.certificate });
    }

    return with_nonce(problem(404, "malformed", "the CA has nothing at this URL"));
}

} // namespace fb::test
