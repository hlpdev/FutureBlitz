#pragma once

#include "fb/runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <psa/crypto.h>
#include <span>
#include <string_view>
#include <vector>

// What the runtime's two files over mbedTLS share: crypto.cpp's hashes, MACs, key derivation,
// random, ciphers, signatures, and key exchange, and crypto_pem.cpp's PEM keys and certificates.
// They're apart so a program that doesn't read PEM links none of mbedTLS's X.509.
namespace fb::crypto {

/// A key's kind, as std.crypto numbers them.
enum key_kind_t : std::uint8_t {
    KEY_ECDSA_P256 = 0,
    KEY_RSA = 1,
    KEY_X25519 = 2,
    KEY_ED25519 = 3,
};

constexpr psa_key_type_t p256_pair = PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1);
constexpr psa_key_type_t p256_public = PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1);
constexpr psa_key_type_t x25519_pair = PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY);

/// The size of an X25519 or Ed25519 key, private or public, and of X25519's secret.
constexpr std::size_t curve_key_size = 32;

/// Starts PSA once, for every thread: its key store and its random generator, which it seeds
/// from the OS. A program that can't have the OS's randomness can't go on safely, so it panics.
void start();

/// Panics when an operation std.crypto checked the arguments of fails anyway, as when memory
/// runs out.
void check(psa_status_t status, const char* what);

std::span<const std::uint8_t> bytes_of(const fb_array_t* array);
std::span<const std::uint8_t> bytes_of(const fb_string_t* text);

/// Makes `array`, a UInt8[], `size` bytes long, and gives where its bytes are, to be written.
std::uint8_t* resized(fb_array_t* array, std::size_t size);

/// Puts `bytes` in `array`, a UInt8[].
void set(fb_array_t* array, std::span<const std::uint8_t> bytes);

fb_string_t* string_of(std::string_view text);

/// What a PSA key, or its public key, exports as, or nothing when it can't be.
std::vector<std::uint8_t> exported(mbedtls_svc_key_id_t id);
std::vector<std::uint8_t> exported_public(mbedtls_svc_key_id_t id);

/// A key in PSA's store, made for one use, which goes when this does.
class stored_key_t {
public:
    /// The key of `data`, for `usage` with `algorithm`.
    stored_key_t(const psa_key_type_t type, const psa_key_usage_t usage,
                 const psa_algorithm_t algorithm, const std::span<const std::uint8_t> data) {
        start();
        psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_type(&attributes, type);
        psa_set_key_usage_flags(&attributes, usage);
        psa_set_key_algorithm(&attributes, algorithm);
        m_status = psa_import_key(&attributes, data.data(), data.size(), &m_id);
        psa_reset_key_attributes(&attributes);
    }

    /// A new random key of `type` and `bits`, which can be exported.
    stored_key_t(const psa_key_type_t type, const std::size_t bits) {
        start();
        psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_type(&attributes, type);
        psa_set_key_bits(&attributes, bits);
        psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_EXPORT);
        m_status = psa_generate_key(&attributes, &m_id);
        psa_reset_key_attributes(&attributes);
    }

    stored_key_t(const stored_key_t&) = delete;
    stored_key_t& operator=(const stored_key_t&) = delete;
    stored_key_t(stored_key_t&&) = delete;
    stored_key_t& operator=(stored_key_t&&) = delete;

    ~stored_key_t() {
        psa_destroy_key(m_id);
    }

    /// Whether the bytes were a key of the type.
    [[nodiscard]] bool made() const {
        return m_status == PSA_SUCCESS;
    }

    [[nodiscard]] mbedtls_svc_key_id_t id() const {
        return m_id;
    }

private:
    mbedtls_svc_key_id_t m_id = MBEDTLS_SVC_KEY_ID_INIT;
    psa_status_t m_status = PSA_ERROR_GENERIC_ERROR;
};

} // namespace fb::crypto
