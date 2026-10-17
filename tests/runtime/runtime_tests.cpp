#include "fb/runtime.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <format>
#include <limits>
#include <string>
#include <vector>

namespace {

/// The text of a String the runtime made, which is released.
std::string take(fb_string_t* string) {
    // The bytes follow the header.
    std::string text(reinterpret_cast<const char*>(string + 1),
                     static_cast<std::size_t>(string->length));
    fb_string_release(string);
    return text;
}

} // namespace

TEST_CASE("numbers become text the way the compiler folds them", "[runtime]") {
    CHECK(take(fb_string_from_int(-42)) == "-42");
    CHECK(take(fb_string_from_uint(std::numeric_limits<std::uint64_t>::max())) ==
          "18446744073709551615");
    CHECK(take(fb_string_from_float32(0.1F)) == "0.1");
    CHECK(take(fb_string_from_float32(0.1F)) == std::format("{}", 0.1F));
    CHECK(take(fb_string_from_float64(0.1)) == "0.1");
    CHECK(take(fb_string_from_float64(1e100)) == std::format("{}", 1e100));
    CHECK(take(fb_string_from_float64(2.5)) == "2.5");
    CHECK(take(fb_string_from_bool(1)) == "True");
    CHECK(take(fb_string_from_bool(0)) == "False");
}

TEST_CASE("Strings join, concatenate, and compare", "[runtime]") {
    fb_string_t* yes = fb_string_from_bool(1);
    fb_string_t* seven = fb_string_from_int(7);
    CHECK(take(fb_string_concat(yes, seven)) == "True7");

    const std::vector<const fb_string_t*> parts{ seven, yes, seven };
    CHECK(take(fb_string_join(parts.data(), static_cast<std::int64_t>(parts.size()))) == "7True7");
    CHECK(take(fb_string_join(parts.data(), 0)).empty());

    fb_string_t* other_seven = fb_string_from_int(7);
    CHECK(fb_string_equal(seven, other_seven) == 1);
    CHECK(fb_string_equal(seven, yes) == 0);
    fb_string_release(other_seven);
    fb_string_release(seven);
    fb_string_release(yes);
}

TEST_CASE("retain and release count references", "[runtime]") {
    fb_string_t* string = fb_string_from_int(5);
    CHECK(string->refcount == 1);
    fb_string_retain(string);
    CHECK(string->refcount == 2);
    fb_string_release(string);
    CHECK(string->refcount == 1);
    fb_string_release(string);
}

TEST_CASE("a literal's negative refcount never changes", "[runtime]") {
    // Laid out like the compiler's String literals: the header, then the bytes.
    struct literal_t {
        fb_string_t header;
        std::array<char, 2> bytes;
    };

    literal_t literal{ .header = { .refcount = -1, .length = 2 }, .bytes = { 'h', 'i' } };
    fb_string_retain(&literal.header);
    fb_string_release(&literal.header);
    fb_string_release(&literal.header);
    CHECK(literal.header.refcount == -1);
    CHECK(take(fb_string_concat(&literal.header, &literal.header)) == "hihi");
}

TEST_CASE("integer powers wrap, or overflow when checked", "[runtime]") {
    // The arguments are base, exponent, bit width, whether overflow panics, and where it panics.
    CHECK(fb_power_int(2, 10, 32, 1, nullptr) == 1024);
    CHECK(fb_power_int(-2, 3, 32, 1, nullptr) == -8);
    CHECK(fb_power_int(-1, 1'000'001, 64, 1, nullptr) == -1);
    CHECK(fb_power_int(7, 0, 8, 1, nullptr) == 1);
    CHECK(fb_power_int(2, 7, 8, 0, nullptr) == -128); // 128 wraps around in Int8
    // 3^40 doesn't fit in Int64, so it wraps to the same bits as the UInt64 value.
    CHECK(fb_power_int(3, 40, 64, 0, nullptr) ==
          static_cast<std::int64_t>(12157665459056928801ULL));
    CHECK(fb_power_uint(2, 63, 64, 1, nullptr) == 9223372036854775808ULL);
    CHECK(fb_power_uint(2, 8, 8, 0, nullptr) == 0); // 256 wraps around in UInt8
    CHECK(fb_power_uint(10, 19, 64, 1, nullptr) == 10000000000000000000ULL);
}

namespace {

/// An array's destroy when its elements need no releasing.
void free_numbers(fb_object_t* object) {
    fb_array_free(reinterpret_cast<fb_array_t*>(object));
}

/// How many times destroy_thing has run.
int destroyed = 0;

void destroy_thing(fb_object_t* /*object*/) {
    ++destroyed;
}

/// An object with one field after the header, laid out the way compiled code lays it out.
struct thing_t {
    fb_object_t header;
    std::int64_t value;
};

/// The Class of a thing_t, with no objects yet.
fb_class_t thing_class() {
    return {
        .name = "Thing",
        .size = sizeof(thing_t),
        .destroy = destroy_thing,
        .live = 0,
        .base = nullptr,
    };
}

} // namespace

TEST_CASE("an object is destroyed when its last owner lets go", "[runtime]") {
    fb_class_t type = thing_class();
    destroyed = 0;
    fb_object_t* object = fb_object_new(&type);
    CHECK(object->refcount == 1);
    CHECK(object->type == &type);
    CHECK(type.live == 1);

    fb_object_retain(object);
    fb_object_release(object);
    CHECK(destroyed == 0);
    fb_object_release(object);
    CHECK(destroyed == 1);
    CHECK(type.live == 0);
}

TEST_CASE("a new object's fields start at zero", "[runtime]") {
    fb_class_t type = thing_class();
    auto* thing = reinterpret_cast<thing_t*>(fb_object_new(&type));
    CHECK(thing->value == 0);
    fb_object_release(&thing->header);
}

TEST_CASE("atomic retain and release count the same way", "[runtime]") {
    fb_class_t type = thing_class();
    destroyed = 0;
    fb_object_t* object = fb_object_new(&type);
    fb_object_retain_atomic(object);
    CHECK(object->refcount == 2);
    fb_object_release_atomic(object);
    fb_object_release_atomic(object);
    CHECK(destroyed == 1);
    CHECK(type.live == 0);
}

TEST_CASE("null objects and Strings are ignored", "[runtime]") {
    // A variable of a Class holds null until it's assigned, and is still released. A String? or
    // Class? holds null for Null.
    fb_object_retain(nullptr);
    fb_object_release(nullptr);
    fb_object_retain_atomic(nullptr);
    fb_object_release_atomic(nullptr);
    fb_string_retain(nullptr);
    fb_string_release(nullptr);
}

TEST_CASE("the String count follows allocations", "[runtime]") {
    const std::int64_t before = fb_string_live_count();
    fb_string_t* string = fb_string_from_int(1);
    CHECK(fb_string_live_count() == before + 1);
    fb_string_release(string);
    CHECK(fb_string_live_count() == before);
}

TEST_CASE("an object is of its Class and the ones it extends", "[runtime]") {
    const fb_class_t base = thing_class();
    fb_class_t type = thing_class();
    type.base = &base;
    const fb_class_t other = thing_class();
    fb_object_t* object = fb_object_new(&type);
    CHECK(fb_object_is(object, &type) == 1);
    CHECK(fb_object_is(object, &base) == 1);
    CHECK(fb_object_is(object, &other) == 0);
    CHECK(fb_object_is(nullptr, &base) == 0);

    // A Class it extends passes; the rest would panic.
    fb_object_expect(object, &base, nullptr);
    fb_object_release(object);
}

namespace {

/// The Class of an array of Int64, whose elements need no releasing.
fb_class_t numbers_class() {
    return {
        .name = "Int64[]",
        .size = sizeof(fb_array_t),
        .destroy = free_numbers,
        .live = 0,
        .base = nullptr,
    };
}

/// The element at `index` of an array of Int64.
std::int64_t& at(const fb_array_t* array, const std::int64_t index) {
    return static_cast<std::int64_t*>(array->data)[index];
}

/// Puts `value` where the runtime made room for it.
void store(void* slot, const std::int64_t value) {
    *static_cast<std::int64_t*>(slot) = value;
}

} // namespace

TEST_CASE("an array grows, and its elements move to make room", "[runtime]") {
    fb_class_t type = numbers_class();
    constexpr std::int64_t size = sizeof(std::int64_t);
    fb_array_t* array = fb_array_new(&type, size, 0);
    CHECK(array->count == 0);
    for (std::int64_t i = 0; i < 10; ++i) {
        store(fb_array_push(array, size), i);
    }

    CHECK(array->count == 10);
    CHECK(array->capacity >= 10);
    store(fb_array_insert(array, size, 0, nullptr), 100);
    CHECK(at(array, 0) == 100);
    CHECK(at(array, 1) == 0);
    fb_array_remove(array, size, 0);
    CHECK(at(array, 0) == 0);
    CHECK(array->count == 10);

    fb_array_t* copy = fb_array_clone(array, size);
    at(copy, 9) = 42;
    CHECK(at(array, 9) == 9);
    fb_array_clear(array);
    CHECK(array->count == 0);
    fb_object_release(&copy->header);
    fb_object_release(&array->header);
    CHECK(type.live == 0);
}
