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

/// A buffer for `count` elements, all zero bytes. It always has room for one, since calloc may
/// give null for none.
void* allocate(const std::int64_t count, const std::int64_t element_size) {
    void* data = std::calloc(static_cast<std::size_t>(std::max<std::int64_t>(count, 1)),
                             static_cast<std::size_t>(element_size));
    if (data == nullptr) {
        fb_panic("out of memory", nullptr);
    }

    return data;
}

/// Makes room for one more element, doubling the capacity when the buffer is full.
void reserve_one(fb_array_t* array, const std::int64_t element_size) {
    if (array->count < array->capacity) {
        return;
    }

    const std::int64_t capacity = std::max<std::int64_t>(4, array->capacity * 2);
    void* data = std::realloc(array->data, bytes(capacity, element_size));
    if (data == nullptr) {
        fb_panic("out of memory", nullptr);
    }

    array->data = data;
    array->capacity = capacity;
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
}
