// std.crypto's XxHash over xxHash: XXH3's 64-bit hash, which is fast and spreads its input well,
// but isn't cryptographic.

#include "fb/crypto.hpp"
#include "fb/runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <xxhash.h>

namespace {

const void* bytes_of(const fb_string_t* text) {
    return text + 1;
}

/// Panics when xxHash fails, which it does only when memory runs out.
void check(const XXH_errorcode result) {
    if (result != XXH_OK) {
        fb_panic("std.crypto's XxHash failed", nullptr);
    }
}

/// A hash of what's added in turn, with the seed it starts again from.
struct hasher_t {
    XXH3_state_t* state;
    std::uint64_t seed;
};

} // namespace

extern "C" {

std::uint64_t fb_xxhash_hash(const fb_array_t* data, const std::uint64_t seed) {
    return XXH3_64bits_withSeed(data->data, static_cast<std::size_t>(data->count), seed);
}

std::uint64_t fb_xxhash_hash_text(const fb_string_t* text, const std::uint64_t seed) {
    return XXH3_64bits_withSeed(bytes_of(text), static_cast<std::size_t>(text->length), seed);
}

void* fb_xxhash_hasher_new(const std::uint64_t seed) {
    XXH3_state_t* state = XXH3_createState();
    if (state == nullptr) {
        fb_panic("out of memory", nullptr);
    }

    check(XXH3_64bits_reset_withSeed(state, seed));
    return new hasher_t{ .state = state, .seed = seed };
}

void fb_xxhash_hasher_add(void* hasher, const fb_array_t* data) {
    check(XXH3_64bits_update(static_cast<hasher_t*>(hasher)->state, data->data,
                             static_cast<std::size_t>(data->count)));
}

void fb_xxhash_hasher_add_text(void* hasher, const fb_string_t* text) {
    check(XXH3_64bits_update(static_cast<hasher_t*>(hasher)->state, bytes_of(text),
                             static_cast<std::size_t>(text->length)));
}

std::uint64_t fb_xxhash_hasher_finish(void* hasher) {
    const auto* xx = static_cast<hasher_t*>(hasher);
    const std::uint64_t hash = XXH3_64bits_digest(xx->state);
    check(XXH3_64bits_reset_withSeed(xx->state, xx->seed));
    return hash;
}

void fb_xxhash_hasher_free(void* hasher) {
    auto* xx = static_cast<hasher_t*>(hasher);
    XXH3_freeState(xx->state);
    delete xx;
}
}
