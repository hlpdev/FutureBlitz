// std.crypto's Ed25519 over Monocypher: a private key is its 32-byte seed, which the key pair,
// its secret key and public key, comes from, and a signature is 64 bytes (RFC 8032).

#include "fb/crypto.hpp"
#include "fb/runtime.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <monocypher-ed25519.h>
#include <monocypher.h>
#include <utility>

namespace {

constexpr std::size_t seed_size = 32;
constexpr std::size_t secret_size = 64;
constexpr std::size_t signature_size = 64;

/// The secret key and the public key of a 32-byte seed. Monocypher wipes the seed it's given,
/// so it's given a copy.
void key_pair(const fb_array_t* seed, std::array<std::uint8_t, secret_size>& secret,
              std::array<std::uint8_t, seed_size>& public_key) {
    std::array<std::uint8_t, seed_size> copy{};
    const auto* bytes = static_cast<const std::uint8_t*>(seed->data);
    std::copy(bytes, bytes + seed_size, copy.begin());
    crypto_ed25519_key_pair(secret.data(), public_key.data(), copy.data());
}

/// Whether a point's 32 bytes are as RFC 8032 encodes it (5.1.3): y, its low 255 bits, below
/// p = 2^255 - 19, and x's sign, the top bit, clear when x is 0, as it is only for y = 1 and
/// y = p - 1. Monocypher takes other encodings of a public key or a signature's R; RFC 8032
/// doesn't.
bool canonical(const std::uint8_t* point) {
    const bool sign = (point[31] & 0x80U) != 0;
    const bool top = (point[31] & 0x7fU) == 0x7fU;
    const bool middle_ff =
        std::all_of(point + 1, point + 31, [](const std::uint8_t byte) { return byte == 0xffU; });
    const bool middle_zero =
        std::all_of(point + 1, point + 31, [](const std::uint8_t byte) { return byte == 0; });
    // p - 1 = 2^255 - 20 is 0xec, then 0xff up to a top byte of 0x7f.
    const bool at_least_p = middle_ff && top && point[0] >= 0xedU;
    const bool minus_one = middle_ff && top && point[0] == 0xecU;
    const bool one = middle_zero && (point[31] & 0x7fU) == 0 && point[0] == 1;
    return !at_least_p && !(sign && (one || minus_one));
}

/// Puts `bytes` in `array`, a UInt8[].
template <std::size_t Size>
void set(fb_array_t* array, const std::array<std::uint8_t, Size>& bytes) {
    fb_array_reserve(array, 1, Size, nullptr);
    array->count = Size;
    std::copy(bytes.begin(), bytes.end(), static_cast<std::uint8_t*>(array->data));
}

} // namespace

extern "C" {

void fb_ed25519_public_key(const fb_array_t* private_key, fb_array_t* output) {
    std::array<std::uint8_t, secret_size> secret{};
    std::array<std::uint8_t, seed_size> public_key{};
    key_pair(private_key, secret, public_key);
    crypto_wipe(secret.data(), secret.size());
    set(output, public_key);
}

void fb_ed25519_sign(const fb_array_t* private_key, const fb_array_t* message, fb_array_t* output) {
    std::array<std::uint8_t, secret_size> secret{};
    std::array<std::uint8_t, seed_size> public_key{};
    key_pair(private_key, secret, public_key);
    std::array<std::uint8_t, signature_size> signature{};
    crypto_ed25519_sign(signature.data(), secret.data(),
                        static_cast<const std::uint8_t*>(message->data),
                        static_cast<std::size_t>(message->count));
    crypto_wipe(secret.data(), secret.size());
    set(output, signature);
}

std::int32_t fb_ed25519_verify(const fb_array_t* public_key, const fb_array_t* message,
                               const fb_array_t* signature) {
    if (std::cmp_not_equal(public_key->count, seed_size) ||
        std::cmp_not_equal(signature->count, signature_size)) {
        return 0;
    }

    const auto* key = static_cast<const std::uint8_t*>(public_key->data);
    const auto* signed_bytes = static_cast<const std::uint8_t*>(signature->data);
    if (!canonical(key) || !canonical(signed_bytes)) {
        return 0;
    }

    return crypto_ed25519_check(signed_bytes, key, static_cast<const std::uint8_t*>(message->data),
                                static_cast<std::size_t>(message->count)) == 0
               ? 1
               : 0;
}
}
