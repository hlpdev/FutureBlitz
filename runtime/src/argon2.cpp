// std.crypto's Argon2id over Argon2's reference code: raw bytes from a password, for a key, and
// the PHC string a password hash is kept as, with what verifies a password against one.

#include "fb/crypto.hpp"
#include "fb/runtime.hpp"

#include <argon2.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

/// A copy of a UInt8[]'s bytes, which Argon2 takes as ones it could change.
std::vector<std::uint8_t> copy_of(const fb_array_t* array) {
    const auto* bytes = static_cast<const std::uint8_t*>(array->data);
    return { bytes, bytes + array->count };
}

/// Panics when Argon2 fails with arguments std.crypto checked, as when memory runs out.
void check(const int result) {
    if (result != ARGON2_OK) {
        std::array<char, 128> message{};
        std::snprintf(message.data(), message.size(), "std.crypto's Argon2id failed: %s",
                      argon2_error_message(result));
        fb_panic(message.data(), nullptr);
    }
}

std::uint32_t to_32(const std::int64_t value) {
    return static_cast<std::uint32_t>(value);
}

} // namespace

extern "C" {

void fb_argon2_derive(const fb_array_t* password, const fb_array_t* salt, const fb_array_t* secret,
                      const fb_array_t* associated, const std::int64_t memory,
                      const std::int64_t iterations, const std::int64_t parallelism,
                      const std::int64_t length, fb_array_t* output) {
    std::vector<std::uint8_t> pwd = copy_of(password);
    std::vector<std::uint8_t> salted = copy_of(salt);
    std::vector<std::uint8_t> key = copy_of(secret);
    std::vector<std::uint8_t> extra = copy_of(associated);
    fb_array_reserve(output, 1, length, nullptr);
    output->count = length;
    argon2_context context{};
    context.out = static_cast<std::uint8_t*>(output->data);
    context.outlen = to_32(length);
    context.pwd = pwd.data();
    context.pwdlen = to_32(static_cast<std::int64_t>(pwd.size()));
    context.salt = salted.data();
    context.saltlen = to_32(static_cast<std::int64_t>(salted.size()));
    context.secret = key.empty() ? nullptr : key.data();
    context.secretlen = to_32(static_cast<std::int64_t>(key.size()));
    context.ad = extra.empty() ? nullptr : extra.data();
    context.adlen = to_32(static_cast<std::int64_t>(extra.size()));
    context.t_cost = to_32(iterations);
    context.m_cost = to_32(memory);
    context.lanes = to_32(parallelism);
    context.threads = to_32(parallelism);
    context.version = ARGON2_VERSION_13;
    context.flags = ARGON2_DEFAULT_FLAGS;
    check(argon2_ctx(&context, Argon2_id));
}

fb_string_t* fb_argon2_hash(const fb_array_t* password, const fb_array_t* salt,
                            const std::int64_t memory, const std::int64_t iterations,
                            const std::int64_t parallelism) {
    constexpr std::size_t hash_length = 32;
    const std::vector<std::uint8_t> pwd = copy_of(password);
    const std::vector<std::uint8_t> salted = copy_of(salt);
    std::string encoded(argon2_encodedlen(to_32(iterations), to_32(memory), to_32(parallelism),
                                          to_32(static_cast<std::int64_t>(salted.size())),
                                          hash_length, Argon2_id),
                        '\0');
    check(argon2id_hash_encoded(to_32(iterations), to_32(memory), to_32(parallelism), pwd.data(),
                                pwd.size(), salted.data(), salted.size(), hash_length,
                                encoded.data(), encoded.size()));
    return fb_string_from_cstring(encoded.c_str());
}

std::int32_t fb_argon2_verify(const fb_array_t* password, const fb_string_t* hash) {
    const std::vector<std::uint8_t> pwd = copy_of(password);
    const std::string encoded(reinterpret_cast<const char*>(hash + 1),
                              static_cast<std::size_t>(hash->length));
    // A hash with a NUL in it isn't one Argon2 made.
    if (encoded.find('\0') != std::string::npos) {
        return 0;
    }

    return argon2id_verify(encoded.c_str(), pwd.data(), pwd.size()) == ARGON2_OK ? 1 : 0;
}
}
