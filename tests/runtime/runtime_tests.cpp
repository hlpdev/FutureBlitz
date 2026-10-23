#include "fb/runtime.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <string>
#include <string_view>
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
    CHECK(take(fb_string_from_float64(2.5)) == "2.5");
    // The invariant text, the same on every machine: no '+' or zeros in an exponent, and NaN and
    // Inf for the floats that aren't numbers.
    CHECK(take(fb_string_from_float64(1e100)) == "1e100");
    CHECK(take(fb_string_from_float64(1.5e-7)) == "1.5e-7");
    CHECK(take(fb_string_from_float64(std::numeric_limits<double>::quiet_NaN())) == "NaN");
    CHECK(take(fb_string_from_float32(-std::numeric_limits<float>::infinity())) == "-Inf");
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

/// An array's release when its elements need no releasing.
void free_numbers(fb_object_t* object) {
    fb_array_free(reinterpret_cast<fb_array_t*>(object));
}

/// How many times destroy_thing and release_thing have run.
int destroyed = 0;
int released = 0;

void destroy_thing(fb_object_t* /*object*/) {
    ++destroyed;
}

void release_thing(fb_object_t* /*object*/) {
    ++released;
}

/// A visit for objects whose fields refer to no objects.
void visit_nothing(const fb_object_t* /*object*/, fb_visitor_t /*visitor*/, void* /*context*/) {}

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
        .release = release_thing,
        .visit = visit_nothing,
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

TEST_CASE("a Weak reference reads as null once its object is gone", "[runtime]") {
    fb_class_t type = thing_class();
    destroyed = 0;
    released = 0;
    fb_object_t* object = fb_object_new(&type);
    fb_object_retain_weak(object);
    fb_object_t* read = fb_object_load_weak(object);
    CHECK(read == object);
    CHECK(object->refcount == 2);
    fb_object_release(read);

    // The memory waits for the Weak reference, which frees it.
    fb_object_release(object);
    CHECK(destroyed == 1);
    CHECK(released == 1);
    CHECK(type.live == 0);
    CHECK(fb_object_load_weak(object) == nullptr);
    fb_object_release_weak(object);
    CHECK(fb_object_load_weak(nullptr) == nullptr);
}

TEST_CASE("an object whose New threw is released without its Destroy", "[runtime]") {
    fb_class_t type = thing_class();
    destroyed = 0;
    released = 0;
    fb_object_discard(fb_object_new(&type));
    CHECK(destroyed == 0);
    CHECK(released == 1);
    CHECK(type.live == 0);

    // One that New gave to something else stays, and is destroyed as usual later.
    fb_object_t* kept = fb_object_new(&type);
    fb_object_retain(kept);
    fb_object_discard(kept);
    CHECK(type.live == 1);
    fb_object_release(kept);
    CHECK(destroyed == 1);
}

TEST_CASE("a new object's fields start at zero", "[runtime]") {
    fb_class_t type = thing_class();
    auto* thing = reinterpret_cast<thing_t*>(fb_object_new(&type));
    CHECK(thing->value == 0);
    fb_object_release(&thing->header);
}

TEST_CASE("a Shared Class's object counts the same way", "[runtime]") {
    fb_class_t type = thing_class();
    type.shared = 1;
    destroyed = 0;
    released = 0;
    fb_object_t* object = fb_object_new(&type);
    // It holds a Weak reference to itself while it lives.
    CHECK(object->weak_count == 1);
    fb_object_retain(object);
    CHECK(object->refcount == 2);
    fb_object_release(object);
    fb_object_release(object);
    CHECK(destroyed == 1);
    CHECK(released == 1);
    CHECK(type.live == 0);

    // A Weak reference keeps the memory, reads as null once the object is gone, and frees it.
    fb_object_t* watched = fb_object_new(&type);
    fb_object_retain_weak(watched);
    fb_object_t* read = fb_object_load_weak(watched);
    CHECK(read == watched);
    fb_object_release(read);
    fb_object_release(watched);
    CHECK(destroyed == 2);
    CHECK(fb_object_load_weak(watched) == nullptr);
    fb_object_release_weak(watched);

    // One whose New threw goes without its Destroy.
    fb_object_discard(fb_object_new(&type));
    CHECK(destroyed == 2);
    CHECK(released == 3);
    CHECK(type.live == 0);
}

TEST_CASE("null objects and Strings are ignored", "[runtime]") {
    // A variable of a Class holds null until it's assigned, and is still released. A String? or
    // Class? holds null for Null.
    fb_object_retain(nullptr);
    fb_object_release(nullptr);
    fb_object_retain_weak(nullptr);
    fb_object_release_weak(nullptr);
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

TEST_CASE("a String goes to C as a counted copy with a NUL after it", "[runtime]") {
    fb_string_t* string = fb_string_from_int(-125);
    const std::int64_t before = fb_cstring_live_count();
    char* copy = fb_cstring_new(string);
    CHECK(fb_cstring_live_count() == before + 1);
    CHECK(std::string_view(copy) == "-125");
    CHECK(copy[4] == '\0');
    fb_cstring_free(copy);
    CHECK(fb_cstring_live_count() == before);
    fb_string_release(string);
}

TEST_CASE("C's text comes back as a String up to its NUL, or null", "[runtime]") {
    CHECK(take(fb_string_from_cstring("abc\0def")) == "abc");
    CHECK(take(fb_string_from_cstring("")).empty());
    CHECK(fb_string_from_cstring(nullptr) == nullptr);
}

TEST_CASE("C's text that isn't UTF-8 gets U+FFFD for each byte that doesn't start a code point",
          "[runtime]") {
    // Well-formed UTF-8 stays as it is, U+FFFD included.
    CHECK(take(fb_string_from_cstring("\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80 \xEF\xBF\xBD")) ==
          "\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80 \xEF\xBF\xBD");

    // A code page's byte, a sequence cut short, an overlong form, a surrogate, and one past
    // U+10FFFF.
    const auto replaced = [](const std::size_t count) {
        std::string text;
        for (std::size_t i = 0; i < count; ++i) {
            text += "\xEF\xBF\xBD";
        }

        return text;
    };
    CHECK(take(fb_string_from_cstring("a\xE9"
                                      "b")) == "a\xEF\xBF\xBD"
                                               "b");
    CHECK(take(fb_string_from_cstring("\xE2\x82")) == replaced(2));
    CHECK(take(fb_string_from_cstring("\xC0\xAF")) == replaced(2));
    CHECK(take(fb_string_from_cstring("\xED\xA0\x80")) == replaced(3));
    CHECK(take(fb_string_from_cstring("\xF4\x90\x80\x80")) == replaced(4));
}

TEST_CASE("Mem's aligned memory is at a multiple of its alignment, and all 0", "[runtime]") {
    for (const std::uint64_t alignment : { 1U, 2U, 8U, 32U, 256U, 4096U, 65536U }) {
        INFO(alignment);
        auto* memory = static_cast<unsigned char*>(fb_mem_alloc_aligned(100, alignment));
        REQUIRE(memory != nullptr);
        CHECK(reinterpret_cast<std::uintptr_t>(memory) % alignment == 0);
        CHECK(
            std::all_of(memory, memory + 100, [](const unsigned char byte) { return byte == 0; }));
        fb_mem_free_aligned(memory);
    }

    // None is a byte, as Alloc's is, and more than there is gives null.
    void* none = fb_mem_alloc_aligned(0, 16);
    CHECK(none != nullptr);
    fb_mem_free_aligned(none);
    fb_mem_free_aligned(nullptr);
    CHECK(fb_mem_alloc_aligned(std::numeric_limits<std::uint64_t>::max() / 4, 16) == nullptr);

    void* uninitialized = fb_mem_alloc_uninitialized(0);
    CHECK(uninitialized != nullptr);
    fb_mem_free(uninitialized);
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

TEST_CASE("an object implements the Interfaces its Class lists, with a table for each",
          "[runtime]") {
    const fb_interface_t shown{ .name = "Shown" };
    const fb_interface_t other{ .name = "Other" };
    int method = 0;
    std::array<void*, 1> methods{ &method };
    const std::array<fb_implementation_t, 1> interfaces{
        { { .interface = &shown, .methods = methods.data() } },
    };
    fb_class_t type = thing_class();
    type.interfaces = interfaces.data();
    type.interface_count = 1;
    fb_object_t* object = fb_object_new(&type);
    CHECK(fb_object_implements(object, &shown) == 1);
    CHECK(fb_object_implements(object, &other) == 0);
    CHECK(fb_object_implements(nullptr, &shown) == 0);
    CHECK(fb_interface_methods(object, &shown) == methods.data());

    // One it implements passes; the rest would panic.
    fb_object_expect_implements(object, &shown, nullptr);
    fb_object_release(object);
}

namespace {

/// The Class of an array of Int64, whose elements need no releasing.
fb_class_t numbers_class() {
    return {
        .name = "Int64[]",
        .size = sizeof(fb_array_t),
        .destroy = nullptr,
        .release = free_numbers,
        .visit = visit_nothing,
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

TEST_CASE("an array's capacity stops at the most an Int counts", "[runtime]") {
    // Elements that take no bytes, like an empty Struct's, need no memory however many there are.
    fb_class_t type = numbers_class();
    fb_array_t* array = fb_array_new(&type, 0, INT32_MAX - 1);
    fb_array_push(array, 0);
    CHECK(array->count == INT32_MAX);
    CHECK(array->capacity == INT32_MAX);
    fb_object_release(&array->header);
    CHECK(type.live == 0);
}

TEST_CASE("Strings order by their bytes, and equal values hash the same", "[runtime]") {
    fb_string_t* apple = fb_string_from_bool(0);
    fb_string_t* same = fb_string_from_bool(0);
    fb_string_t* other = fb_string_from_bool(1);
    // "False" comes before "True".
    CHECK(fb_string_compare(apple, other) < 0);
    CHECK(fb_string_compare(other, apple) > 0);
    CHECK(fb_string_compare(apple, same) == 0);
    CHECK(fb_string_hash(apple) == fb_string_hash(same));
    CHECK(fb_string_hash(apple) != fb_string_hash(other));
    fb_string_release(apple);
    fb_string_release(same);
    fb_string_release(other);

    CHECK(fb_hash_integer(1) != fb_hash_integer(2));
    CHECK(fb_hash_float(0.0) == fb_hash_float(-0.0));
    CHECK(fb_hash_combine(fb_hash_integer(1), fb_hash_integer(2)) !=
          fb_hash_combine(fb_hash_integer(2), fb_hash_integer(1)));
}

namespace {

/// A String like the compiler's literals: a static block that's never freed, its bytes right
/// after the header.
struct literal_t {
    fb_string_t header;
    std::array<char, 10> bytes;
};

} // namespace

TEST_CASE("a String's Chars are its code points, and its Bytes are its UTF-8", "[runtime]") {
    // "aé€😀" has characters of one, two, three, and four bytes.
    constexpr std::string_view text = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80";
    literal_t literal{ .header = { .refcount = -1, .length = 10 }, .bytes = {} };
    std::ranges::copy(text, literal.bytes.begin());
    fb_class_t type = numbers_class();
    fb_array_t* chars = fb_array_new(&type, sizeof(std::int32_t), 0);
    fb_string_chars(&literal.header, chars);
    const std::vector<std::int32_t> points(static_cast<const std::int32_t*>(chars->data),
                                           static_cast<const std::int32_t*>(chars->data) +
                                               chars->count);
    CHECK(points == std::vector<std::int32_t>{ 0x61, 0xE9, 0x20AC, 0x1F600 });

    fb_array_t* bytes = fb_array_new(&type, 1, 0);
    fb_string_bytes(&literal.header, bytes);
    CHECK(std::string_view(static_cast<const char*>(bytes->data),
                           static_cast<std::size_t>(bytes->count)) == text);
    fb_object_release(&chars->header);
    fb_object_release(&bytes->header);
    CHECK(type.live == 0);
}

TEST_CASE("std.text's functions read a String by byte position", "[runtime]") {
    // "aé€😀a" has characters of one, two, three, and four bytes, and an "a" at either end.
    fb_string_t* text = fb_string_from_cstring("a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80"
                                               "a");
    fb_string_t* a = fb_string_from_cstring("a");
    fb_string_t* euro = fb_string_from_cstring("\xE2\x82\xAC");
    fb_string_t* empty = fb_string_from_cstring("");
    CHECK(fb_text_length(text) == 11);
    CHECK(fb_text_byte(text, 1) == 0xC3);
    CHECK(fb_text_char_at(text, 1) == 0xE9);
    CHECK(fb_text_char_at(text, 6) == 0x1F600);
    CHECK(fb_text_char_count(text, 0, 11) == 5);
    CHECK(fb_text_char_count(text, 3, 10) == 2);

    // Find looks from a position on, and FindLast for one that ends at a position or sooner.
    CHECK(fb_text_find(text, a, 0) == 0);
    CHECK(fb_text_find(text, a, 1) == 10);
    CHECK(fb_text_find(text, euro, 4) == -1);
    CHECK(fb_text_find(text, empty, 6) == 6);
    CHECK(fb_text_find_last(text, a, 11) == 10);
    CHECK(fb_text_find_last(text, a, 10) == 0);
    CHECK(fb_text_find_last(text, euro, 5) == -1);
    CHECK(fb_text_find_last(text, euro, 6) == 3);
    CHECK(fb_text_find_last(text, text, 10) == -1);

    // What's at a position can be part of a character, or run past the end.
    CHECK(fb_text_matches_at(text, euro, 3) == 1);
    CHECK(fb_text_matches_at(text, euro, 2) == 0);
    CHECK(fb_text_matches_at(text, a, 10) == 1);
    CHECK(fb_text_matches_at(text, a, 11) == 0);
    CHECK(fb_text_matches_at(text, empty, 11) == 1);
    CHECK(fb_text_matches_at(a, text, 0) == 0);

    CHECK(take(fb_text_slice(text, 1, 6)) == "\xC3\xA9\xE2\x82\xAC");
    CHECK(take(fb_text_slice(text, 6, 6)).empty());
    CHECK(take(fb_text_repeat(euro, 3)) == "\xE2\x82\xAC\xE2\x82\xAC\xE2\x82\xAC");
    CHECK(take(fb_text_repeat(euro, 0)).empty());
    for (fb_string_t* string : { text, a, euro, empty }) {
        fb_string_release(string);
    }
}

TEST_CASE("std.text's bytes are a String only when they're well-formed UTF-8", "[runtime]") {
    fb_class_t type = numbers_class();
    fb_array_t* bytes = fb_array_new(&type, 1, 0);
    fb_string_t* text = fb_string_from_cstring("\xC3\xA9\xF0\x9F\x98\x80");
    fb_text_append(bytes, text);
    fb_text_append(bytes, text);
    CHECK(bytes->count == 12);
    CHECK(take(fb_text_from_utf8(bytes, 0, 12)) ==
          "\xC3\xA9\xF0\x9F\x98\x80\xC3\xA9\xF0\x9F\x98\x80");
    CHECK(take(fb_text_from_utf8(bytes, 2, 4)) == "\xF0\x9F\x98\x80");
    // A count that cuts a character short, or starts inside one, isn't UTF-8.
    CHECK(fb_text_from_utf8(bytes, 0, 5) == nullptr);
    CHECK(fb_text_from_utf8(bytes, 1, 2) == nullptr);
    CHECK(take(fb_text_from_utf8(bytes, 0, 0)).empty());

    // Join puts the separator between each two parts.
    fb_array_t* parts = fb_array_new(&type, sizeof(fb_string_t*), 0);
    fb_string_t* comma = fb_string_from_cstring(", ");
    CHECK(take(fb_text_join(parts, comma)).empty());
    for (int i = 0; i < 3; ++i) {
        *static_cast<fb_string_t**>(fb_array_push(parts, sizeof(fb_string_t*))) = text;
    }

    CHECK(take(fb_text_join(parts, comma)) ==
          "\xC3\xA9\xF0\x9F\x98\x80, \xC3\xA9\xF0\x9F\x98\x80, \xC3\xA9\xF0\x9F\x98\x80");
    fb_string_release(comma);
    fb_string_release(text);
    fb_object_release(&parts->header);
    fb_object_release(&bytes->header);
    CHECK(type.live == 0);
}
