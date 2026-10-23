// std.crypto's Bcrypt over Solar Designer's crypt_blowfish: a password's "$2b$" hash, with a salt
// and a cost, and what verifies a password against a hash of any of bcrypt's versions.

#include "fb/crypto.hpp"
#include "fb/runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

extern "C" {
#include <crypt_blowfish.h>
}

namespace {

/// The size of a bcrypt hash, its NUL included, and of its setting, the part before the hash.
constexpr std::size_t hash_size = 61;
constexpr std::size_t setting_size = 30;

std::string text_of(const fb_string_t* text) {
    return { reinterpret_cast<const char*>(text + 1), static_cast<std::size_t>(text->length) };
}

} // namespace

extern "C" {

fb_string_t* fb_bcrypt_hash(const fb_string_t* password, const fb_array_t* salt,
                            const std::int32_t cost) {
    std::array<char, setting_size> setting{};
    if (_crypt_gensalt_blowfish_rn("$2b$", static_cast<unsigned long>(cost),
                                   static_cast<const char*>(salt->data),
                                   static_cast<int>(salt->count), setting.data(),
                                   static_cast<int>(setting.size())) == nullptr) {
        fb_panic("std.crypto's Bcrypt couldn't make a setting from its salt", nullptr);
    }

    const std::string key = text_of(password);
    std::array<char, hash_size> hash{};
    if (_crypt_blowfish_rn(key.c_str(), setting.data(), hash.data(),
                           static_cast<int>(hash.size())) == nullptr) {
        fb_panic("std.crypto's Bcrypt failed", nullptr);
    }

    return fb_string_from_cstring(hash.data());
}

std::int32_t fb_bcrypt_verify(const fb_string_t* password, const fb_string_t* hash) {
    const std::string key = text_of(password);
    const std::string expected = text_of(hash);
    // crypt_blowfish reads C strings, which a NUL would end early, so neither can have one. Only
    // bcrypt's own versions are a hash: crypt_blowfish's $2x$ keeps an old bug for bytes from
    // 0x80, which gives many passwords one hash.
    const bool bcrypt = expected.starts_with("$2a$") || expected.starts_with("$2b$") ||
                        expected.starts_with("$2y$");
    if (key.contains('\0') || expected.size() + 1 != hash_size || !bcrypt) {
        return 0;
    }

    std::array<char, hash_size> computed{};
    if (_crypt_blowfish_rn(key.c_str(), expected.c_str(), computed.data(),
                           static_cast<int>(computed.size())) == nullptr) {
        return 0;
    }

    // Compared without stopping at the first difference.
    unsigned difference = 0;
    for (std::size_t i = 0; i + 1 < hash_size; ++i) {
        difference |= static_cast<unsigned>(static_cast<unsigned char>(computed[i])) ^
                      static_cast<unsigned>(static_cast<unsigned char>(expected[i]));
    }

    return difference == 0 ? 1 : 0;
}
}
