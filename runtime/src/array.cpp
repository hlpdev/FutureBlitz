#include "fb/runtime.hpp"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Arrays: objects whose elements live in a buffer of their own, which grows as they're added.
// Compiled code stores, retains, and releases the elements, since only it knows their type.

namespace {

/// The size in bytes of `count` elements.
std::size_t bytes(const std::int64_t count, const std::int64_t element_size) {
    return static_cast<std::size_t>(count) * static_cast<std::size_t>(element_size);
}

/// A buffer for `count` elements, all zero bytes. It always has a byte or more, since calloc
/// may give null for none, as an empty array or a Struct without Fields would ask.
void* allocate(const std::int64_t count, const std::int64_t element_size) {
    const std::int64_t room = element_size == 0 ? 1 : std::max<std::int64_t>(count, 1);
    void* data = std::calloc(static_cast<std::size_t>(room),
                             static_cast<std::size_t>(std::max<std::int64_t>(element_size, 1)));
    if (data == nullptr) {
        fb_panic("out of memory", nullptr);
    }

    return data;
}

/// Moves the elements to a buffer for `capacity` of them, which must hold the count. Reserve
/// gives the place it was called from, which a panic for memory it can't have says, with how
/// many elements it asked for.
void resize_buffer(fb_array_t* array, const std::int64_t element_size, const std::int64_t capacity,
                   const fb_location_t* reserved = nullptr) {
    const auto out_of_memory = [&] {
        if (reserved == nullptr) {
            fb_panic("out of memory", nullptr);
        }

        std::array<char, 96> message{};
        std::snprintf(message.data(), message.size(),
                      "out of memory: Reserve couldn't make room for %" PRId64 " elements",
                      capacity);
        fb_panic(message.data(), reserved);
    };

    // More bytes than memory has would wrap around.
    if (element_size > 0 && capacity > PTRDIFF_MAX / element_size) {
        out_of_memory();
    }

    void* data = std::realloc(array->data, std::max<std::size_t>(bytes(capacity, element_size), 1));
    if (data == nullptr) {
        out_of_memory();
    }

    array->data = data;
    array->capacity = capacity;
}

/// The most elements an array holds, since its Count and Capacity are Ints.
constexpr std::int64_t max_count = INT32_MAX;

/// Makes room for one more element, doubling the capacity when the buffer is full, up to the
/// most an array holds, past which it panics.
void reserve_one(fb_array_t* array, const std::int64_t element_size) {
    if (array->count < array->capacity) {
        return;
    }

    if (array->count >= max_count) {
        fb_panic("an array holds at most 2147483647 elements", nullptr);
    }

    resize_buffer(array, element_size,
                  std::min(max_count, std::max<std::int64_t>(4, array->capacity * 2)));
}

/// Where the element at `index` is.
void* element(const fb_array_t* array, const std::int64_t element_size, const std::int64_t index) {
    return static_cast<char*>(array->data) + bytes(index, element_size);
}

} // namespace

extern "C" {

fb_array_t* fb_array_new(fb_class_t* type, const std::int64_t element_size,
                         const std::int64_t count) {
    auto* array = reinterpret_cast<fb_array_t*>(fb_object_new(type));
    array->count = count;
    array->capacity = count;
    array->data = allocate(count, element_size);
    return array;
}

void* fb_array_push(fb_array_t* array, const std::int64_t element_size) {
    reserve_one(array, element_size);
    return element(array, element_size, array->count++);
}

void* fb_array_insert(fb_array_t* array, const std::int64_t element_size, const std::int64_t index,
                      const fb_location_t* location) {
    // Inserting at the count adds to the end.
    if (index < 0 || index > array->count) {
        fb_index_out_of_range(index, array->count, location);
    }

    reserve_one(array, element_size);
    std::memmove(element(array, element_size, index + 1), element(array, element_size, index),
                 bytes(array->count - index, element_size));
    ++array->count;
    return element(array, element_size, index);
}

void fb_array_remove(fb_array_t* array, const std::int64_t element_size, const std::int64_t index) {
    std::memmove(element(array, element_size, index), element(array, element_size, index + 1),
                 bytes(array->count - index - 1, element_size));
    --array->count;
}

void fb_array_clear(fb_array_t* array) {
    array->count = 0;
}

void fb_array_reserve(fb_array_t* array, const std::int64_t element_size,
                      const std::int64_t capacity, const fb_location_t* location) {
    if (capacity < 0) {
        std::array<char, 96> message{};
        std::snprintf(message.data(), message.size(),
                      "Reserve takes a capacity that isn't negative, not %" PRId64, capacity);
        fb_panic(message.data(), location);
    }

    if (capacity > array->capacity) {
        resize_buffer(array, element_size, capacity, location);
    }
}

void fb_array_trim(fb_array_t* array, const std::int64_t element_size) {
    if (array->capacity > array->count) {
        resize_buffer(array, element_size, array->count);
    }
}

fb_array_t* fb_array_clone(const fb_array_t* array, const std::int64_t element_size) {
    fb_array_t* copy = fb_array_new(array->header.type, element_size, array->count);
    std::memcpy(copy->data, array->data, bytes(array->count, element_size));
    return copy;
}

void fb_array_free(const fb_array_t* array) {
    std::free(array->data);
}

void fb_index_out_of_range(const std::int64_t index, const std::int64_t count,
                           const fb_location_t* location) {
    std::array<char, 128> message{};
    std::snprintf(message.data(), message.size(),
                  "index %" PRId64 " is out of range for %" PRId64 " element%s", index, count,
                  count == 1 ? "" : "s");
    fb_panic(message.data(), location);
}

void fb_unsigned_index_out_of_range(const std::uint64_t index, const std::int64_t count,
                                    const fb_location_t* location) {
    std::array<char, 128> message{};
    std::snprintf(message.data(), message.size(),
                  "index %" PRIu64 " is out of range for %" PRId64 " element%s", index, count,
                  count == 1 ? "" : "s");
    fb_panic(message.data(), location);
}
}
