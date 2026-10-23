// std.crypto's functions over mbedTLS: hashes, HMAC, HKDF, PBKDF2, secure random, ciphers,
// signatures, and key exchange, through TF-PSA-Crypto's PSA API, whose key store and random
// generator any thread can use. Each operation imports the key it's given and destroys it after,
// so nothing stays in the key store. crypto_pem.cpp has PEM keys and certificates.

#include "fb/crypto.hpp"

#include "crypto_internal.hpp"
#include "fb/runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <psa/crypto.h>
#include <span>
#include <string_view>
#include <vector>

// mbedTLS's headers without C++'s guard: its constant-time comparison, and its RSA module, which
// reads RSA keys as PKCS #1's DER.
extern "C" {
#include <mbedtls/constant_time.h>
#include <rsa_internal.h>
}

using fb::crypto::bytes_of;
using fb::crypto::check;
using fb::crypto::curve_key_size;
using fb::crypto::exported;
using fb::crypto::exported_public;
using fb::crypto::p256_pair;
using fb::crypto::p256_public;
using fb::crypto::put_length;
using fb::crypto::resized;
using fb::crypto::set;
using fb::crypto::start;
using fb::crypto::stored_key_t;
using fb::crypto::x25519_pair;

namespace fb::crypto {

void start() {
    static const psa_status_t started = psa_crypto_init();
    if (started != PSA_SUCCESS) {
        fb_panic("std.crypto can't start, since the OS gives no random bytes", nullptr);
    }
}

void check(const psa_status_t status, const char* what) {
    if (status != PSA_SUCCESS) {
        std::array<char, 128> message{};
        std::snprintf(message.data(), message.size(), "std.crypto's %s failed with PSA error %d",
                      what, static_cast<int>(status));
        fb_panic(message.data(), nullptr);
    }
}

std::span<const std::uint8_t> bytes_of(const fb_array_t* array) {
    return { static_cast<const std::uint8_t*>(array->data),
             static_cast<std::size_t>(array->count) };
}

std::span<const std::uint8_t> bytes_of(const fb_string_t* text) {
    return { reinterpret_cast<const std::uint8_t*>(text + 1),
             static_cast<std::size_t>(text->length) };
}

std::uint8_t* resized(fb_array_t* array, const std::size_t size) {
    fb_array_reserve(array, 1, static_cast<std::int64_t>(size), nullptr);
    array->count = static_cast<std::int64_t>(size);
    return static_cast<std::uint8_t*>(array->data);
}

void set(fb_array_t* array, const std::span<const std::uint8_t> bytes) {
    if (!bytes.empty()) {
        std::memcpy(resized(array, bytes.size()), bytes.data(), bytes.size());
    } else {
        array->count = 0;
    }
}

fb_string_t* string_of(const std::string_view text) {
    return fb_string_from_bytes(text.data(), static_cast<std::int64_t>(text.size()));
}

std::vector<std::uint8_t> exported(const mbedtls_svc_key_id_t id) {
    std::vector<std::uint8_t> bytes(PSA_EXPORT_KEY_PAIR_MAX_SIZE);
    std::size_t size = 0;
    if (psa_export_key(id, bytes.data(), bytes.size(), &size) != PSA_SUCCESS) {
        return {};
    }

    bytes.resize(size);
    return bytes;
}

std::vector<std::uint8_t> exported_public(const mbedtls_svc_key_id_t id) {
    std::vector<std::uint8_t> bytes(PSA_EXPORT_PUBLIC_KEY_MAX_SIZE);
    std::size_t size = 0;
    if (psa_export_public_key(id, bytes.data(), bytes.size(), &size) != PSA_SUCCESS) {
        return {};
    }

    bytes.resize(size);
    return bytes;
}

void put_length(std::vector<std::uint8_t>& der, const std::size_t length) {
    if (length < 0x80) {
        der.push_back(static_cast<std::uint8_t>(length));
        return;
    }

    std::vector<std::uint8_t> bytes;
    for (std::size_t rest = length; rest > 0; rest >>= 8U) {
        bytes.insert(bytes.begin(), static_cast<std::uint8_t>(rest & 0xffU));
    }

    der.push_back(static_cast<std::uint8_t>(0x80U | bytes.size()));
    der.insert(der.end(), bytes.begin(), bytes.end());
}

} // namespace fb::crypto

namespace {

/// The size of an AEAD nonce and tag, which AES-256-GCM and ChaCha20-Poly1305 share.
constexpr std::size_t nonce_size = 12;
constexpr std::size_t tag_size = 16;

psa_algorithm_t hash_algorithm(const std::int32_t algorithm) {
    switch (algorithm) {
    case 1:
        return PSA_ALG_MD5;
    case 2:
        return PSA_ALG_SHA_1;
    case 3:
        return PSA_ALG_SHA_256;
    case 4:
        return PSA_ALG_SHA_512;
    default:
        fb_panic("std.crypto was given a hash it doesn't have", nullptr);
    }
}

/// An HMAC key as PSA takes it, which gives the same MAC. One longer than the hash's block is its
/// hash, in `hashed`, as RFC 2104 has HMAC do anyway, since PSA takes no key over 8191 bytes. One
/// of no bytes is one 0, which HMAC pads alike, since PSA takes no empty key.
std::span<const std::uint8_t> hmac_key(const psa_algorithm_t hash,
                                       const std::span<const std::uint8_t> key,
                                       std::array<std::uint8_t, PSA_HASH_MAX_SIZE>& hashed) {
    static constexpr std::array<std::uint8_t, 1> zero{};
    if (key.empty()) {
        return zero;
    }
    if (key.size() <= PSA_HASH_BLOCK_LENGTH(hash)) {
        return key;
    }

    std::size_t size = 0;
    check(psa_hash_compute(hash, key.data(), key.size(), hashed.data(), hashed.size(), &size),
          "HMAC");
    return std::span<const std::uint8_t>(hashed).first(size);
}

/// Whether `signature` is `public_key`'s, an RSA key as PKCS #1's DER, of `message`'s SHA-256: by
/// RSASSA-PKCS1-v1_5, or by RSASSA-PSS with MGF1 by SHA-256 and a salt as long as the hash, as
/// RFC 7518's PS256 has it, and only that. mbedTLS's RSA module verifies it rather than PSA,
/// which takes no key of an odd number of bits.
bool rsa_verified(const bool pss, const std::span<const std::uint8_t> public_key,
                  const std::span<const std::uint8_t> message,
                  const std::span<const std::uint8_t> signature) {
    start();
    std::array<std::uint8_t, PSA_HASH_LENGTH(PSA_ALG_SHA_256)> hash{};
    std::size_t size = 0;
    check(psa_hash_compute(PSA_ALG_SHA_256, message.data(), message.size(), hash.data(),
                           hash.size(), &size),
          "Verify");
    mbedtls_rsa_context rsa;
    mbedtls_rsa_init(&rsa);
    bool verified = mbedtls_rsa_parse_pubkey(&rsa, public_key.data(), public_key.size()) == 0 &&
                    mbedtls_rsa_check_pubkey(&rsa) == 0 &&
                    signature.size() == mbedtls_rsa_get_len(&rsa);
    if (verified && pss) {
        verified = mbedtls_rsa_rsassa_pss_verify_ext(
                       &rsa, MBEDTLS_MD_SHA256, hash.size(), hash.data(), MBEDTLS_MD_SHA256,
                       static_cast<int>(hash.size()), signature.data()) == 0;
    } else if (verified) {
        verified = mbedtls_rsa_pkcs1_verify(&rsa, MBEDTLS_MD_SHA256, hash.size(), hash.data(),
                                            signature.data()) == 0;
    }

    mbedtls_rsa_free(&rsa);
    return verified;
}

/// A DER INTEGER of big-endian `value`, which isn't negative.
void put_integer(std::vector<std::uint8_t>& der, std::span<const std::uint8_t> value) {
    while (value.size() > 1 && value.front() == 0) {
        value = value.subspan(1);
    }

    const bool high = !value.empty() && (value.front() & 0x80U) != 0;
    der.push_back(0x02);
    put_length(der, value.size() + (high ? 1 : 0) + (value.empty() ? 1 : 0));
    if (high || value.empty()) {
        der.push_back(0);
    }

    der.insert(der.end(), value.begin(), value.end());
}

/// A hasher over PSA's hash operation, which starts again after each hash it gives.
struct hasher_t {
    psa_algorithm_t algorithm;
    psa_hash_operation_t operation;
};

} // namespace

extern "C" {

void fb_crypto_hash(const std::int32_t algorithm, const fb_array_t* data, fb_array_t* output) {
    start();
    const psa_algorithm_t hash = hash_algorithm(algorithm);
    const std::span<const std::uint8_t> input = bytes_of(data);
    std::size_t size = 0;
    check(psa_hash_compute(hash, input.data(), input.size(), resized(output, PSA_HASH_LENGTH(hash)),
                           PSA_HASH_LENGTH(hash), &size),
          "hash");
}

void fb_crypto_hash_text(const std::int32_t algorithm, const fb_string_t* text,
                         fb_array_t* output) {
    start();
    const psa_algorithm_t hash = hash_algorithm(algorithm);
    const std::span<const std::uint8_t> input = bytes_of(text);
    std::size_t size = 0;
    check(psa_hash_compute(hash, input.data(), input.size(), resized(output, PSA_HASH_LENGTH(hash)),
                           PSA_HASH_LENGTH(hash), &size),
          "hash");
}

void* fb_crypto_hasher_new(const std::int32_t algorithm) {
    start();
    auto* hasher = new hasher_t{
        .algorithm = hash_algorithm(algorithm),
        .operation = PSA_HASH_OPERATION_INIT,
    };
    check(psa_hash_setup(&hasher->operation, hasher->algorithm), "hash");
    return hasher;
}

void fb_crypto_hasher_add(void* hasher, const fb_array_t* data) {
    const std::span<const std::uint8_t> input = bytes_of(data);
    check(psa_hash_update(&static_cast<hasher_t*>(hasher)->operation, input.data(), input.size()),
          "hash");
}

void fb_crypto_hasher_add_text(void* hasher, const fb_string_t* text) {
    const std::span<const std::uint8_t> input = bytes_of(text);
    check(psa_hash_update(&static_cast<hasher_t*>(hasher)->operation, input.data(), input.size()),
          "hash");
}

void fb_crypto_hasher_finish(void* hasher, fb_array_t* output) {
    auto* state = static_cast<hasher_t*>(hasher);
    const std::size_t length = PSA_HASH_LENGTH(state->algorithm);
    std::size_t size = 0;
    check(psa_hash_finish(&state->operation, resized(output, length), length, &size), "hash");
    check(psa_hash_setup(&state->operation, state->algorithm), "hash");
}

void fb_crypto_hasher_free(void* hasher) {
    auto* state = static_cast<hasher_t*>(hasher);
    psa_hash_abort(&state->operation);
    delete state;
}

void fb_crypto_hmac(const std::int32_t algorithm, const fb_array_t* key, const fb_array_t* data,
                    fb_array_t* output) {
    start();
    const psa_algorithm_t hmac = PSA_ALG_HMAC(hash_algorithm(algorithm));
    std::array<std::uint8_t, PSA_HASH_MAX_SIZE> hashed{};
    const stored_key_t made(PSA_KEY_TYPE_HMAC, PSA_KEY_USAGE_SIGN_MESSAGE, hmac,
                            hmac_key(hash_algorithm(algorithm), bytes_of(key), hashed));
    check(made.made() ? PSA_SUCCESS : PSA_ERROR_INVALID_ARGUMENT, "HMAC");
    const std::span<const std::uint8_t> input = bytes_of(data);
    const std::size_t length = PSA_HASH_LENGTH(hash_algorithm(algorithm));
    std::size_t size = 0;
    check(psa_mac_compute(made.id(), hmac, input.data(), input.size(), resized(output, length),
                          length, &size),
          "HMAC");
}

void fb_crypto_hkdf(const std::int32_t algorithm, const fb_array_t* material,
                    const fb_array_t* salt, const fb_array_t* info, const std::int64_t length,
                    fb_array_t* output) {
    start();
    psa_key_derivation_operation_t operation = PSA_KEY_DERIVATION_OPERATION_INIT;
    const std::span<const std::uint8_t> secret = bytes_of(material);
    const std::span<const std::uint8_t> salted = bytes_of(salt);
    const std::span<const std::uint8_t> context = bytes_of(info);
    check(psa_key_derivation_setup(&operation, PSA_ALG_HKDF(hash_algorithm(algorithm))), "HKDF");
    check(psa_key_derivation_input_bytes(&operation, PSA_KEY_DERIVATION_INPUT_SALT, salted.data(),
                                         salted.size()),
          "HKDF");
    check(psa_key_derivation_input_bytes(&operation, PSA_KEY_DERIVATION_INPUT_SECRET, secret.data(),
                                         secret.size()),
          "HKDF");
    check(psa_key_derivation_input_bytes(&operation, PSA_KEY_DERIVATION_INPUT_INFO, context.data(),
                                         context.size()),
          "HKDF");
    const auto size = static_cast<std::size_t>(length);
    check(psa_key_derivation_output_bytes(&operation, resized(output, size), size), "HKDF");
    psa_key_derivation_abort(&operation);
}

void fb_crypto_pbkdf2(const std::int32_t algorithm, const fb_array_t* password,
                      const fb_array_t* salt, const std::int64_t iterations,
                      const std::int64_t length, fb_array_t* output) {
    start();
    psa_key_derivation_operation_t operation = PSA_KEY_DERIVATION_OPERATION_INIT;
    const std::span<const std::uint8_t> secret = bytes_of(password);
    const std::span<const std::uint8_t> salted = bytes_of(salt);
    check(psa_key_derivation_setup(&operation, PSA_ALG_PBKDF2_HMAC(hash_algorithm(algorithm))),
          "PBKDF2");
    check(psa_key_derivation_input_integer(&operation, PSA_KEY_DERIVATION_INPUT_COST,
                                           static_cast<std::uint64_t>(iterations)),
          "PBKDF2");
    check(psa_key_derivation_input_bytes(&operation, PSA_KEY_DERIVATION_INPUT_SALT, salted.data(),
                                         salted.size()),
          "PBKDF2");
    check(psa_key_derivation_input_bytes(&operation, PSA_KEY_DERIVATION_INPUT_PASSWORD,
                                         secret.data(), secret.size()),
          "PBKDF2");
    const auto size = static_cast<std::size_t>(length);
    check(psa_key_derivation_output_bytes(&operation, resized(output, size), size), "PBKDF2");
    psa_key_derivation_abort(&operation);
}

void fb_crypto_random(const std::int64_t count, fb_array_t* output) {
    start();
    const auto size = static_cast<std::size_t>(count);
    check(psa_generate_random(resized(output, size), size), "random");
}

std::int32_t fb_crypto_equal(const fb_array_t* a, const fb_array_t* b) {
    if (a->count != b->count) {
        return 0;
    }

    return mbedtls_ct_memcmp(a->data, b->data, static_cast<std::size_t>(a->count)) == 0 ? 1 : 0;
}

void fb_crypto_seal(const std::int32_t cipher, const fb_array_t* key, const fb_array_t* message,
                    const fb_array_t* associated, fb_array_t* output) {
    const psa_algorithm_t algorithm = cipher == 1 ? PSA_ALG_GCM : PSA_ALG_CHACHA20_POLY1305;
    const psa_key_type_t type = cipher == 1 ? PSA_KEY_TYPE_AES : PSA_KEY_TYPE_CHACHA20;
    const stored_key_t made(type, PSA_KEY_USAGE_ENCRYPT, algorithm, bytes_of(key));
    check(made.made() ? PSA_SUCCESS : PSA_ERROR_INVALID_ARGUMENT, "Seal");
    const std::span<const std::uint8_t> plain = bytes_of(message);
    const std::span<const std::uint8_t> extra = bytes_of(associated);
    const std::size_t total = nonce_size + plain.size() + tag_size;
    std::uint8_t* sealed = resized(output, total);
    check(psa_generate_random(sealed, nonce_size), "random");
    std::size_t size = 0;
    check(psa_aead_encrypt(made.id(), algorithm, sealed, nonce_size, extra.data(), extra.size(),
                           plain.data(), plain.size(), sealed + nonce_size, total - nonce_size,
                           &size),
          "Seal");
}

std::int32_t fb_crypto_open(const std::int32_t cipher, const fb_array_t* key,
                            const fb_array_t* sealed, const fb_array_t* associated,
                            fb_array_t* output) {
    const std::span<const std::uint8_t> input = bytes_of(sealed);
    if (input.size() < nonce_size + tag_size) {
        return 0;
    }

    const psa_algorithm_t algorithm = cipher == 1 ? PSA_ALG_GCM : PSA_ALG_CHACHA20_POLY1305;
    const psa_key_type_t type = cipher == 1 ? PSA_KEY_TYPE_AES : PSA_KEY_TYPE_CHACHA20;
    const stored_key_t made(type, PSA_KEY_USAGE_DECRYPT, algorithm, bytes_of(key));
    check(made.made() ? PSA_SUCCESS : PSA_ERROR_INVALID_ARGUMENT, "Open");
    const std::span<const std::uint8_t> extra = bytes_of(associated);
    // Into a buffer of its own first, so a message that doesn't authenticate leaves no bytes.
    std::vector<std::uint8_t> plain(input.size() - nonce_size - tag_size);
    std::size_t size = 0;
    const psa_status_t status = psa_aead_decrypt(
        made.id(), algorithm, input.data(), nonce_size, extra.data(), extra.size(),
        input.data() + nonce_size, input.size() - nonce_size, plain.data(), plain.size(), &size);
    if (status == PSA_ERROR_INVALID_SIGNATURE) {
        return 0;
    }

    check(status, "Open");
    set(output, std::span(plain).first(size));
    return 1;
}

void fb_crypto_key_new(const std::int32_t kind, fb_array_t* output) {
    const bool p256 = kind == fb::crypto::KEY_ECDSA_P256;
    const stored_key_t made(p256 ? p256_pair : x25519_pair, p256 ? 256 : 255);
    check(made.made() ? PSA_SUCCESS : PSA_ERROR_GENERIC_ERROR, "new key");
    set(output, exported(made.id()));
}

std::int32_t fb_crypto_public_key(const std::int32_t kind, const fb_array_t* private_key,
                                  fb_array_t* output) {
    const stored_key_t made(kind == fb::crypto::KEY_ECDSA_P256 ? p256_pair : x25519_pair, 0,
                            PSA_ALG_NONE, bytes_of(private_key));
    if (!made.made()) {
        return 0;
    }

    set(output, exported_public(made.id()));
    return 1;
}

std::int32_t fb_crypto_sign(const fb_array_t* private_key, const fb_array_t* message,
                            fb_array_t* output) {
    const psa_algorithm_t algorithm = PSA_ALG_DETERMINISTIC_ECDSA(PSA_ALG_SHA_256);
    const stored_key_t made(p256_pair, PSA_KEY_USAGE_SIGN_MESSAGE, algorithm,
                            bytes_of(private_key));
    if (!made.made()) {
        return 0;
    }

    const std::span<const std::uint8_t> input = bytes_of(message);
    const std::size_t length = PSA_SIGN_OUTPUT_SIZE(p256_pair, 256, algorithm);
    std::size_t size = 0;
    check(psa_sign_message(made.id(), algorithm, input.data(), input.size(),
                           resized(output, length), length, &size),
          "Sign");
    return 1;
}

std::int32_t fb_crypto_verify(const std::int32_t scheme, const fb_array_t* public_key,
                              const fb_array_t* message, const fb_array_t* signature) {
    const std::span<const std::uint8_t> input = bytes_of(message);
    const std::span<const std::uint8_t> signed_bytes = bytes_of(signature);
    if (scheme == 2 || scheme == 3) {
        return rsa_verified(scheme == 3, bytes_of(public_key), input, signed_bytes) ? 1 : 0;
    }

    const psa_algorithm_t algorithm = PSA_ALG_ECDSA(PSA_ALG_SHA_256);
    const stored_key_t made(p256_public, PSA_KEY_USAGE_VERIFY_MESSAGE, algorithm,
                            bytes_of(public_key));
    if (!made.made()) {
        return 0;
    }

    return psa_verify_message(made.id(), algorithm, input.data(), input.size(), signed_bytes.data(),
                              signed_bytes.size()) == PSA_SUCCESS
               ? 1
               : 0;
}

void fb_crypto_rsa_key(const fb_array_t* modulus, const fb_array_t* exponent, fb_array_t* output) {
    std::vector<std::uint8_t> integers;
    put_integer(integers, bytes_of(modulus));
    put_integer(integers, bytes_of(exponent));
    std::vector<std::uint8_t> der{ 0x30 };
    put_length(der, integers.size());
    der.insert(der.end(), integers.begin(), integers.end());
    set(output, der);
}

std::int32_t fb_crypto_shared_secret(const fb_array_t* private_key, const fb_array_t* public_key,
                                     fb_array_t* output) {
    const stored_key_t made(x25519_pair, PSA_KEY_USAGE_DERIVE, PSA_ALG_ECDH, bytes_of(private_key));
    if (!made.made()) {
        return 0;
    }

    const std::span<const std::uint8_t> peer = bytes_of(public_key);
    std::array<std::uint8_t, curve_key_size> secret{};
    constexpr std::array<std::uint8_t, curve_key_size> zero{};
    std::size_t size = 0;
    if (psa_raw_key_agreement(PSA_ALG_ECDH, made.id(), peer.data(), peer.size(), secret.data(),
                              secret.size(), &size) != PSA_SUCCESS ||
        mbedtls_ct_memcmp(secret.data(), zero.data(), secret.size()) == 0) {
        return 0;
    }

    set(output, secret);
    return 1;
}
}
