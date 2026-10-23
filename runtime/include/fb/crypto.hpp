#pragma once

#include "fb/runtime.hpp"

#include <cstdint>

// std.crypto's functions in the runtime, each over the bundled C library its prefix names, which
// a program links only when it calls one: fb_crypto_ over mbedTLS, fb_xxhash_ over xxHash,
// fb_blake3_ over BLAKE3, fb_argon2_ over Argon2, fb_bcrypt_ over crypt_blowfish, and
// fb_ed25519_ over Monocypher. Arrays and
// Strings are only borrowed. What gives bytes puts them in `output`, a UInt8[] the caller made,
// whose old bytes they replace. std.crypto checks sizes and counts first, so a bad one here is a
// bug, which panics.
//
// The numbers std.crypto passes: a hash is 1 MD5, 2 SHA-1, 3 SHA-256, or 4 SHA-512; a cipher is
// 1 AES-256-GCM or 2 ChaCha20-Poly1305; a key's kind is 0 ECDSA P-256, 1 RSA, 2 X25519, or
// 3 Ed25519; and a signature is 1 ECDSA P-256 with SHA-256, 2 RSA PKCS #1 v1.5 with SHA-256, or
// 3 RSA PSS with SHA-256 and a 32-byte salt.

extern "C" {

/// The hash of `data` (a UInt8[]) or `text`'s bytes, by `algorithm`, into `output`.
void fb_crypto_hash(std::int32_t algorithm, const fb_array_t* data, fb_array_t* output);
void fb_crypto_hash_text(std::int32_t algorithm, const fb_string_t* text, fb_array_t* output);
/// A hash by `algorithm` of what's added to it in turn, which fb_crypto_hasher_free gives back.
void* fb_crypto_hasher_new(std::int32_t algorithm);
void fb_crypto_hasher_add(void* hasher, const fb_array_t* data);
void fb_crypto_hasher_add_text(void* hasher, const fb_string_t* text);
/// The hash of what was added, into `output`, after which the hasher starts again, empty.
void fb_crypto_hasher_finish(void* hasher, fb_array_t* output);
void fb_crypto_hasher_free(void* hasher);

/// HMAC by the hash `algorithm`, with `key`, of `data`, into `output`.
void fb_crypto_hmac(std::int32_t algorithm, const fb_array_t* key, const fb_array_t* data,
                    fb_array_t* output);
/// HKDF (RFC 5869) by the hash `algorithm`: `length` bytes from `material`, `salt`, and `info`.
void fb_crypto_hkdf(std::int32_t algorithm, const fb_array_t* material, const fb_array_t* salt,
                    const fb_array_t* info, std::int64_t length, fb_array_t* output);
/// PBKDF2 (RFC 8018) with HMAC by the hash `algorithm`: `length` bytes from `password` and
/// `salt`, in `iterations` rounds, 1 or more.
void fb_crypto_pbkdf2(std::int32_t algorithm, const fb_array_t* password, const fb_array_t* salt,
                      std::int64_t iterations, std::int64_t length, fb_array_t* output);

/// `count` bytes from the OS's randomness, through mbedTLS's generator, into `output`.
void fb_crypto_random(std::int64_t count, fb_array_t* output);
/// Whether `a` and `b` have the same bytes, in a time that depends only on their counts.
std::int32_t fb_crypto_equal(const fb_array_t* a, const fb_array_t* b);

/// `message` encrypted by `cipher` with `key`, as a new random nonce, then the ciphertext, then
/// the tag, which authenticates the ciphertext and `associated` together, into `output`.
void fb_crypto_seal(std::int32_t cipher, const fb_array_t* key, const fb_array_t* message,
                    const fb_array_t* associated, fb_array_t* output);
/// The message that fb_crypto_seal sealed into `sealed`, into `output`, and 1; or 0 when
/// `sealed` and `associated` don't authenticate with `key`.
std::int32_t fb_crypto_open(std::int32_t cipher, const fb_array_t* key, const fb_array_t* sealed,
                            const fb_array_t* associated, fb_array_t* output);

/// A new private key of `kind`, ECDSA P-256's or X25519's, into `output`.
void fb_crypto_key_new(std::int32_t kind, fb_array_t* output);
/// The public key of `kind`'s private key, into `output`, and 1; or 0 when it isn't a key.
std::int32_t fb_crypto_public_key(std::int32_t kind, const fb_array_t* private_key,
                                  fb_array_t* output);
/// The ECDSA P-256 signature of `message`'s SHA-256, deterministic (RFC 6979), r then s, into
/// `output`, and 1; or 0 when `private_key` isn't a key.
std::int32_t fb_crypto_sign(const fb_array_t* private_key, const fb_array_t* message,
                            fb_array_t* output);
/// Whether `signature`, by `scheme`, is `public_key`'s of `message`.
std::int32_t fb_crypto_verify(std::int32_t scheme, const fb_array_t* public_key,
                              const fb_array_t* message, const fb_array_t* signature);
/// An RSA public key, as PKCS #1's DER, of `modulus` and `exponent`, big-endian, into `output`.
void fb_crypto_rsa_key(const fb_array_t* modulus, const fb_array_t* exponent, fb_array_t* output);
/// X25519's shared secret of `private_key` and `public_key`, into `output`, and 1; or 0 when
/// either isn't a key, or the secret is all 0, as a public key of a small order gives.
std::int32_t fb_crypto_shared_secret(const fb_array_t* private_key, const fb_array_t* public_key,
                                     fb_array_t* output);

/// The first key in `pem`, its bytes as std.crypto takes its kind's, into `output`, and its
/// kind times 2, plus 1 for a private key; or -1 when `pem` has none, or -2 when its key is of a
/// kind std.crypto doesn't take, or is encrypted.
std::int32_t fb_crypto_key_from_pem(const fb_string_t* pem, fb_array_t* output);
/// The PEM of a key of `kind`, private when `is_private` isn't 0, or null when `key` isn't one.
fb_string_t* fb_crypto_key_to_pem(std::int32_t kind, std::int32_t is_private,
                                  const fb_array_t* key);

/// The certificates in `pem`, which fb_crypto_certificates_free gives back, or null when it has
/// none, or one that doesn't parse.
void* fb_crypto_certificates_new(const fb_string_t* pem);
std::int64_t fb_crypto_certificates_count(const void* certificates);
/// The certificate at `index`'s subject or issuer, as "C=NL, O=Example, CN=example.com".
fb_string_t* fb_crypto_certificate_subject(const void* certificates, std::int64_t index);
fb_string_t* fb_crypto_certificate_issuer(const void* certificates, std::int64_t index);
/// Its serial number, as hex bytes with colons between them: "01:2F:A0".
fb_string_t* fb_crypto_certificate_serial(const void* certificates, std::int64_t index);
/// When it starts or stops being valid, in UTC: "2026-10-23T12:00:00Z".
fb_string_t* fb_crypto_certificate_not_before(const void* certificates, std::int64_t index);
fb_string_t* fb_crypto_certificate_not_after(const void* certificates, std::int64_t index);
/// Its DER, into `output`.
void fb_crypto_certificate_der(const void* certificates, std::int64_t index, fb_array_t* output);
/// Its public key, as fb_crypto_key_from_pem gives one.
std::int32_t fb_crypto_certificate_key(const void* certificates, std::int64_t index,
                                       fb_array_t* output);
void fb_crypto_certificates_free(void* certificates);

/// XXH3's 64-bit hash of `data` or `text`'s bytes, with `seed`.
std::uint64_t fb_xxhash_hash(const fb_array_t* data, std::uint64_t seed);
std::uint64_t fb_xxhash_hash_text(const fb_string_t* text, std::uint64_t seed);
/// An XXH3 64-bit hash with `seed` of what's added to it in turn, which fb_xxhash_hasher_free
/// gives back. Finish gives the hash of what was added, and the hasher starts again.
void* fb_xxhash_hasher_new(std::uint64_t seed);
void fb_xxhash_hasher_add(void* hasher, const fb_array_t* data);
void fb_xxhash_hasher_add_text(void* hasher, const fb_string_t* text);
std::uint64_t fb_xxhash_hasher_finish(void* hasher);
void fb_xxhash_hasher_free(void* hasher);

/// A BLAKE3 hash of what's added to it in turn, by `mode`: 0 a plain one, 1 a keyed one with
/// `key`, 32 bytes, or 2 one that derives a key for `context`. Finish puts `length` bytes of the
/// hash into `output`, after which the hasher starts again.
void* fb_blake3_hasher_new(std::int32_t mode, const fb_array_t* key, const fb_string_t* context);
void fb_blake3_hasher_add(void* hasher, const fb_array_t* data);
void fb_blake3_hasher_add_text(void* hasher, const fb_string_t* text);
void fb_blake3_hasher_finish(void* hasher, std::int64_t length, fb_array_t* output);
void fb_blake3_hasher_free(void* hasher);

/// Argon2id's `length` bytes from `password`, `salt`, `secret`, and `associated`, with
/// `memory` KiB, `iterations` passes, and `parallelism` lanes, into `output`.
void fb_argon2_derive(const fb_array_t* password, const fb_array_t* salt, const fb_array_t* secret,
                      const fb_array_t* associated, std::int64_t memory, std::int64_t iterations,
                      std::int64_t parallelism, std::int64_t length, fb_array_t* output);
/// The PHC string of `password`'s Argon2id hash, 32 bytes, with `salt` and those parameters:
/// "$argon2id$v=19$m=19456,t=2,p=1$<salt>$<hash>".
fb_string_t* fb_argon2_hash(const fb_array_t* password, const fb_array_t* salt, std::int64_t memory,
                            std::int64_t iterations, std::int64_t parallelism);
/// Whether `password` is what `hash`, a PHC string fb_argon2_hash gave, was made from.
std::int32_t fb_argon2_verify(const fb_array_t* password, const fb_string_t* hash);

/// `password`'s bcrypt hash, "$2b$<cost>$<salt and hash>", with `salt`, 16 bytes, and `cost`,
/// from 4 to 31, which makes 2 to the cost rounds.
fb_string_t* fb_bcrypt_hash(const fb_string_t* password, const fb_array_t* salt, std::int32_t cost);
/// Whether `password` is what `hash`, a bcrypt hash of any of its versions, was made from.
std::int32_t fb_bcrypt_verify(const fb_string_t* password, const fb_string_t* hash);

/// The Ed25519 public key, 32 bytes, of `private_key`, a 32-byte seed, into `output`.
void fb_ed25519_public_key(const fb_array_t* private_key, fb_array_t* output);
/// `message`'s Ed25519 signature with `private_key`, 64 bytes (RFC 8032), into `output`.
void fb_ed25519_sign(const fb_array_t* private_key, const fb_array_t* message, fb_array_t* output);
/// Whether `signature` is `public_key`'s Ed25519 signature of `message`.
std::int32_t fb_ed25519_verify(const fb_array_t* public_key, const fb_array_t* message,
                               const fb_array_t* signature);
}
