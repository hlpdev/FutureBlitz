// std.crypto's PEM keys and X.509 certificates over mbedTLS: its pk module reads and writes ECDSA
// P-256, RSA, and X25519 keys, as PKCS #8, SEC 1, PKCS #1, and SubjectPublicKeyInfo have them,
// and this file reads and writes Ed25519's, which mbedTLS doesn't know, and X25519's when it
// writes them; its X.509 module reads certificates.

#include "crypto_internal.hpp"
#include "fb/crypto.hpp"
#include "fb/runtime.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mbedtls/pem.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using fb::crypto::bytes_of;
using fb::crypto::curve_key_size;
using fb::crypto::exported;
using fb::crypto::exported_public;
using fb::crypto::KEY_ECDSA_P256;
using fb::crypto::KEY_ED25519;
using fb::crypto::KEY_RSA;
using fb::crypto::key_t;
using fb::crypto::KEY_X25519;
using fb::crypto::p256_pair;
using fb::crypto::p256_public;
using fb::crypto::set;
using fb::crypto::start;
using fb::crypto::string_of;

namespace {

/// What fb_crypto_key_from_pem gives for a text with no key, or one std.crypto doesn't take.
constexpr std::int32_t no_key = -1;
constexpr std::int32_t unknown_key = -2;

/// What starts an Ed25519 or X25519 key in DER, before its 32 bytes: PKCS #8's for a private key,
/// or SubjectPublicKeyInfo's for a public one, with its OID, 1.3.101.112 or 1.3.101.110.
std::vector<std::uint8_t> curve_prefix(const std::int32_t kind, const bool is_private) {
    const std::uint8_t oid = kind == KEY_ED25519 ? 0x70 : 0x6e;
    // A SEQUENCE, and a private key's version, 0; then the AlgorithmIdentifier, a SEQUENCE of the
    // OID; then the key, a BIT STRING, or a private key's OCTET STRING in an OCTET STRING.
    std::vector<std::uint8_t> prefix{ 0x30, 0x2a };
    std::vector<std::uint8_t> key{ 0x03, 0x21, 0x00 };
    if (is_private) {
        prefix = { 0x30, 0x2e, 0x02, 0x01, 0x00 };
        key = { 0x04, 0x22, 0x04, 0x20 };
    }

    const std::array<std::uint8_t, 7> algorithm{ 0x30, 0x05, 0x06, 0x03, 0x2b, 0x65, oid };
    prefix.insert(prefix.end(), algorithm.begin(), algorithm.end());
    prefix.insert(prefix.end(), key.begin(), key.end());
    return prefix;
}

/// The PEM block `label`'s DER in `text`, or nothing when it has none.
std::vector<std::uint8_t> pem_block(const std::string& text, const std::string& label) {
    mbedtls_pem_context pem;
    mbedtls_pem_init(&pem);
    const std::string header = "-----BEGIN " + label + "-----";
    const std::string footer = "-----END " + label + "-----";
    std::size_t used = 0;
    std::vector<std::uint8_t> der;
    if (mbedtls_pem_read_buffer(&pem, header.c_str(), footer.c_str(),
                                reinterpret_cast<const unsigned char*>(text.c_str()), nullptr, 0,
                                &used) == 0) {
        std::size_t size = 0;
        const unsigned char* data = mbedtls_pem_get_buffer(&pem, &size);
        der.assign(data, data + size);
    }

    mbedtls_pem_free(&pem);
    return der;
}

/// An Ed25519 or X25519 key in `text`, as fb_crypto_key_from_pem gives one, or no_key.
std::int32_t curve_key_from_pem(const std::string& text, fb_array_t* output) {
    for (const bool is_private : { true, false }) {
        const std::vector<std::uint8_t> der =
            pem_block(text, is_private ? "PRIVATE KEY" : "PUBLIC KEY");
        for (const std::int32_t kind : { KEY_ED25519, KEY_X25519 }) {
            std::vector<std::uint8_t> expected = curve_prefix(kind, is_private);
            const std::size_t size = expected.size() + curve_key_size;
            if (der.size() < size) {
                continue;
            }

            // A private key in PKCS #8's second version (RFC 5958) has version 1, and can have
            // attributes and the public key after the private one, which its SEQUENCE's length
            // counts; it's otherwise the first's.
            const bool second = is_private && der[4] == 1;
            if (second) {
                expected[1] = der[1];
                expected[4] = 1;
            }

            const bool sized =
                der.size() == size || (second && der[1] < 0x80 && der.size() == der[1] + 2U);
            if (sized && std::equal(expected.begin(), expected.end(), der.begin())) {
                set(output, std::span(der).subspan(expected.size(), curve_key_size));
                return kind * 2 + (is_private ? 1 : 0);
            }
        }
    }

    return no_key;
}

/// A key in `pk`, which mbedTLS read, as fb_crypto_key_from_pem gives one: an RSA key as PKCS
/// #1's DER, a P-256 private key as its 32 bytes and a public one as its point, uncompressed, 65
/// bytes, and an X25519 key as its 32 bytes.
std::int32_t pk_key(const mbedtls_pk_context& pk, fb_array_t* output) {
    const psa_key_type_t type = mbedtls_pk_get_key_type(&pk);
    std::int32_t kind = unknown_key;
    if (PSA_KEY_TYPE_IS_RSA(type)) {
        kind = KEY_RSA;
    } else if (PSA_KEY_TYPE_IS_ECC(type) &&
               PSA_KEY_TYPE_ECC_GET_FAMILY(type) == PSA_ECC_FAMILY_SECP_R1 &&
               mbedtls_pk_get_bitlen(&pk) == 256) {
        kind = KEY_ECDSA_P256;
    } else if (PSA_KEY_TYPE_IS_ECC(type) &&
               PSA_KEY_TYPE_ECC_GET_FAMILY(type) == PSA_ECC_FAMILY_MONTGOMERY &&
               mbedtls_pk_get_bitlen(&pk) == 255) {
        kind = KEY_X25519;
    } else {
        return unknown_key;
    }

    // Attributes for signing or verifying let the key be exported too.
    const bool is_private = PSA_KEY_TYPE_IS_KEY_PAIR(type);
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    if (mbedtls_pk_get_psa_attributes(
            &pk, is_private ? PSA_KEY_USAGE_SIGN_MESSAGE : PSA_KEY_USAGE_VERIFY_MESSAGE,
            &attributes) != 0) {
        return unknown_key;
    }

    mbedtls_svc_key_id_t id = MBEDTLS_SVC_KEY_ID_INIT;
    const int imported = mbedtls_pk_import_into_psa(&pk, &attributes, &id);
    psa_reset_key_attributes(&attributes);
    if (imported != 0) {
        return unknown_key;
    }

    const std::vector<std::uint8_t> bytes = is_private ? exported(id) : exported_public(id);
    psa_destroy_key(id);
    if (bytes.empty()) {
        return unknown_key;
    }

    set(output, bytes);
    return kind * 2 + (is_private ? 1 : 0);
}

/// `der` as PEM, under `label`.
fb_string_t* pem_of(const std::span<const std::uint8_t> der, const std::string& label) {
    const std::string header = "-----BEGIN " + label + "-----\n";
    const std::string footer = "-----END " + label + "-----\n";
    std::vector<unsigned char> text(der.size() * 2 + header.size() + footer.size() + 64);
    std::size_t size = 0;
    if (mbedtls_pem_write_buffer(header.c_str(), footer.c_str(), der.data(), der.size(),
                                 text.data(), text.size(), &size) != 0) {
        return nullptr;
    }

    // The size counts the NUL at the end.
    return fb_string_from_bytes(reinterpret_cast<const char*>(text.data()),
                                static_cast<std::int64_t>(size - 1));
}

/// A certificate in the chain mbedTLS parsed, by its index.
const mbedtls_x509_crt& certificate(const void* certificates, const std::int64_t index) {
    const auto* at = static_cast<const mbedtls_x509_crt*>(certificates);
    for (std::int64_t i = 0; i < index; ++i) {
        at = at->next;
    }

    return *at;
}

fb_string_t* name_of(const mbedtls_x509_name& name) {
    std::array<char, 1024> text{};
    const int size = mbedtls_x509_dn_gets(text.data(), text.size(), &name);
    return string_of(size < 0 ? std::string_view() : std::string_view(text.data()));
}

fb_string_t* time_of(const mbedtls_x509_time& time) {
    std::array<char, 32> text{};
    std::snprintf(text.data(), text.size(), "%04d-%02d-%02dT%02d:%02d:%02dZ", time.year, time.mon,
                  time.day, time.hour, time.min, time.sec);
    return string_of(text.data());
}

} // namespace

extern "C" {

std::int32_t fb_crypto_key_from_pem(const fb_string_t* pem, fb_array_t* output) {
    start();
    // mbedTLS reads PEM from a C string, NUL and all.
    const std::span<const std::uint8_t> bytes = bytes_of(pem);
    const std::string text(bytes.begin(), bytes.end());
    const auto* data = reinterpret_cast<const unsigned char*>(text.c_str());
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    std::int32_t found = no_key;
    if (mbedtls_pk_parse_key(&pk, data, text.size() + 1, nullptr, 0) == 0 ||
        mbedtls_pk_parse_public_key(&pk, data, text.size() + 1) == 0) {
        found = pk_key(pk, output);
    } else {
        found = curve_key_from_pem(text, output);
        // A PEM key that mbedTLS can't read, of another curve or encrypted, is one std.crypto
        // doesn't take.
        if (found == no_key && text.find("-----BEGIN ") != std::string::npos &&
            text.find(" KEY-----") != std::string::npos) {
            found = unknown_key;
        }
    }

    mbedtls_pk_free(&pk);
    return found;
}

fb_string_t* fb_crypto_key_to_pem(const std::int32_t kind, const std::int32_t is_private,
                                  const fb_array_t* key) {
    const std::span<const std::uint8_t> bytes = bytes_of(key);
    if (kind == KEY_ED25519 || kind == KEY_X25519) {
        if (bytes.size() != curve_key_size) {
            return nullptr;
        }

        std::vector<std::uint8_t> der = curve_prefix(kind, is_private != 0);
        der.insert(der.end(), bytes.begin(), bytes.end());
        return pem_of(der, is_private != 0 ? "PRIVATE KEY" : "PUBLIC KEY");
    }

    psa_key_type_t type = PSA_KEY_TYPE_RSA_PUBLIC_KEY;
    if (kind == KEY_RSA && is_private != 0) {
        type = PSA_KEY_TYPE_RSA_KEY_PAIR;
    } else if (kind == KEY_ECDSA_P256) {
        type = is_private != 0 ? p256_pair : p256_public;
    }

    const key_t made(type, PSA_KEY_USAGE_EXPORT, PSA_ALG_NONE, bytes);
    if (!made.made()) {
        return nullptr;
    }

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    std::vector<unsigned char> text(16384);
    const int copied = is_private != 0 ? mbedtls_pk_copy_from_psa(made.id(), &pk)
                                       : mbedtls_pk_copy_public_from_psa(made.id(), &pk);
    int written = copied;
    if (copied == 0) {
        written = is_private != 0 ? mbedtls_pk_write_key_pem(&pk, text.data(), text.size())
                                  : mbedtls_pk_write_pubkey_pem(&pk, text.data(), text.size());
    }

    mbedtls_pk_free(&pk);
    if (written != 0) {
        return nullptr;
    }

    return fb_string_from_cstring(reinterpret_cast<const char*>(text.data()));
}

void* fb_crypto_certificates_new(const fb_string_t* pem) {
    start();
    const std::span<const std::uint8_t> bytes = bytes_of(pem);
    const std::string text(bytes.begin(), bytes.end());
    auto* chain = new mbedtls_x509_crt;
    mbedtls_x509_crt_init(chain);
    // A positive result counts the certificates that didn't parse, which is a failure too.
    if (mbedtls_x509_crt_parse(chain, reinterpret_cast<const unsigned char*>(text.c_str()),
                               text.size() + 1) != 0 ||
        chain->raw.len == 0) {
        mbedtls_x509_crt_free(chain);
        delete chain;
        return nullptr;
    }

    return chain;
}

std::int64_t fb_crypto_certificates_count(const void* certificates) {
    std::int64_t count = 0;
    for (const auto* at = static_cast<const mbedtls_x509_crt*>(certificates); at != nullptr;
         at = at->next) {
        ++count;
    }

    return count;
}

fb_string_t* fb_crypto_certificate_subject(const void* certificates, const std::int64_t index) {
    return name_of(certificate(certificates, index).subject);
}

fb_string_t* fb_crypto_certificate_issuer(const void* certificates, const std::int64_t index) {
    return name_of(certificate(certificates, index).issuer);
}

fb_string_t* fb_crypto_certificate_serial(const void* certificates, const std::int64_t index) {
    // DER puts a 0 before a serial whose top bit is set, which isn't the number's.
    const mbedtls_x509_buf& serial = certificate(certificates, index).serial;
    std::size_t first = 0;
    while (first + 1 < serial.len && serial.p[first] == 0) {
        ++first;
    }

    std::string text;
    for (std::size_t i = first; i < serial.len; ++i) {
        std::array<char, 4> digits{};
        std::snprintf(digits.data(), digits.size(), i == first ? "%02X" : ":%02X", serial.p[i]);
        text += digits.data();
    }

    return string_of(text);
}

fb_string_t* fb_crypto_certificate_not_before(const void* certificates, const std::int64_t index) {
    return time_of(certificate(certificates, index).valid_from);
}

fb_string_t* fb_crypto_certificate_not_after(const void* certificates, const std::int64_t index) {
    return time_of(certificate(certificates, index).valid_to);
}

void fb_crypto_certificate_der(const void* certificates, const std::int64_t index,
                               fb_array_t* output) {
    const mbedtls_x509_buf& raw = certificate(certificates, index).raw;
    set(output, std::span<const std::uint8_t>(raw.p, raw.len));
}

std::int32_t fb_crypto_certificate_key(const void* certificates, const std::int64_t index,
                                       fb_array_t* output) {
    return pk_key(certificate(certificates, index).pk, output);
}

void fb_crypto_certificates_free(void* certificates) {
    auto* chain = static_cast<mbedtls_x509_crt*>(certificates);
    mbedtls_x509_crt_free(chain);
    delete chain;
}
}
