// std.crypto's Blake3 over BLAKE3's C code: a plain hash, a keyed one, and one that derives a key
// for a context, any of them of any length.

#include "fb/crypto.hpp"
#include "fb/runtime.hpp"

#include <algorithm>
#include <array>
#include <blake3.h>
#include <cstddef>
#include <cstdint>
#include <string>

namespace {

/// A BLAKE3 hasher, and what starts it again after each hash: its key, or its context.
struct hasher_t {
    blake3_hasher hasher;
    bool keyed = false;
    bool derives = false;
    std::array<std::uint8_t, BLAKE3_KEY_LEN> key{};
    std::string context;
};

void restart(hasher_t& state) {
    if (state.keyed) {
        blake3_hasher_init_keyed(&state.hasher, state.key.data());
    } else if (state.derives) {
        blake3_hasher_init_derive_key_raw(&state.hasher, state.context.data(),
                                          state.context.size());
    } else {
        blake3_hasher_init(&state.hasher);
    }
}

} // namespace

extern "C" {

void* fb_blake3_hasher_new(const std::int32_t mode, const fb_array_t* key,
                           const fb_string_t* context) {
    auto* state = new hasher_t;
    if (mode == 1) {
        state->keyed = true;
        const auto* bytes = static_cast<const std::uint8_t*>(key->data);
        std::copy(bytes, bytes + BLAKE3_KEY_LEN, state->key.begin());
    } else if (mode == 2) {
        state->derives = true;
        state->context.assign(reinterpret_cast<const char*>(context + 1),
                              static_cast<std::size_t>(context->length));
    }

    restart(*state);
    return state;
}

void fb_blake3_hasher_add(void* hasher, const fb_array_t* data) {
    blake3_hasher_update(&static_cast<hasher_t*>(hasher)->hasher, data->data,
                         static_cast<std::size_t>(data->count));
}

void fb_blake3_hasher_add_text(void* hasher, const fb_string_t* text) {
    blake3_hasher_update(&static_cast<hasher_t*>(hasher)->hasher, text + 1,
                         static_cast<std::size_t>(text->length));
}

void fb_blake3_hasher_finish(void* hasher, const std::int64_t length, fb_array_t* output) {
    auto* state = static_cast<hasher_t*>(hasher);
    fb_array_reserve(output, 1, length, nullptr);
    output->count = length;
    blake3_hasher_finalize(&state->hasher, static_cast<std::uint8_t*>(output->data),
                           static_cast<std::size_t>(length));
    restart(*state);
}

void fb_blake3_hasher_free(void* hasher) {
    delete static_cast<hasher_t*>(hasher);
}
}
