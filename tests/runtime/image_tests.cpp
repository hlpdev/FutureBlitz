// std.image's runtime parts on input made to make them hold memory, measured by the most of the
// heap operator new holds while they run: a GIF frame whose descriptor is far larger than its
// screen holds no more than the screen.

#include "fb/image.hpp"
#include "fb/runtime.hpp"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <vector>

#ifdef __APPLE__
#include <malloc/malloc.h>
#else
#include <malloc.h>
#endif

namespace {

/// The bytes operator new holds, and the most it's held since the last start().
std::atomic<std::int64_t> held{ 0 };
std::atomic<std::int64_t> most{ 0 };

/// A block's size, as malloc has it. A block of the C++ library's, which on some targets has
/// an operator new of its own, is counted by the same, so either frees what the other gave.
std::size_t size_of(void* block) {
#ifdef _WIN32
    return _msize(block);
#elifdef __APPLE__
    return malloc_size(block);
#else
    return malloc_usable_size(block);
#endif
}

/// Starts measuring, from what's held now.
std::int64_t start() {
    const std::int64_t now = held.load();
    most.store(now);
    return now;
}

/// An array of the bytes, as std.image passes the runtime a UInt8[].
fb_array_t array_of(std::vector<std::uint8_t>& bytes) {
    return {
        .header = {},
        .count = static_cast<std::int64_t>(bytes.size()),
        .capacity = static_cast<std::int64_t>(bytes.size()),
        .data = bytes.data(),
    };
}

} // namespace

// Every test's operator new and delete count what they hold.
void* operator new(const std::size_t size) {
    void* block = std::malloc(size == 0 ? 1 : size);
    if (block == nullptr) {
        throw std::bad_alloc();
    }

    const std::int64_t now = held += static_cast<std::int64_t>(size_of(block));
    std::int64_t before = most.load();
    while (now > before && !most.compare_exchange_weak(before, now)) {}
    return block;
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
void operator delete(void* pointer) noexcept {
    if (pointer == nullptr) {
        return;
    }

    held -= static_cast<std::int64_t>(size_of(pointer));
    std::free(pointer);
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
void operator delete(void* pointer, std::size_t /*size*/) noexcept {
    operator delete(pointer);
}

TEST_CASE("a GIF frame far larger than its screen holds no more memory than the screen") {
    // A 1 by 1 GIF whose frame says it's 65535 by 65535, with one pixel's codes.
    std::vector<std::uint8_t> gif{
        0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x01, 0x00, 0x01, 0x00, 0x80, 0x00,
        0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0x2c, 0x00, 0x00, 0x00, 0x00,
        0xff, 0xff, 0xff, 0xff, 0x00, 0x02, 0x02, 0x4c, 0x01, 0x00, 0x3b,
    };
    const fb_array_t data = array_of(gif);
    const std::int64_t before = start();
    void* image = fb_image_gif_decode(&data, 1, 100);
    const std::int64_t most_held = most.load() - before;
    CHECK(fb_image_result(image) == 0);
    CHECK(fb_image_width(image) == 1);
    CHECK(fb_image_height(image) == 1);
    fb_image_free(image);
    // Less than a megabyte, where the descriptor would have it 4 GB.
    CHECK(most_held < 1'000'000);
}
