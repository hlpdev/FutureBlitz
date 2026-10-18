#pragma once

#include <cstdint>

// The C ABI that every compiled program links against. compiler/src/codegen.cpp declares these
// functions again in LLVM IR, so a signature change goes in both places.
extern "C" {

/// The header of a String. Its `length` bytes of UTF-8 follow right after it, with no
/// terminating NUL. Strings are immutable and shared by counting references. Any thread can
/// hold one, so the count changes atomically.
struct fb_string_t {
    /// How many owners the String has. A literal is a static block with -1, which retain and
    /// release leave alone, so it's never freed.
    std::int64_t refcount;
    /// In bytes, not characters.
    std::int64_t length;
};

struct fb_object_t;

/// What a Class's visit calls with each object that `context`'s object refers to strongly, and
/// the field it's in, or null for an element of a T[]. `object` can be null.
using fb_visitor_t = void (*)(void* context, const fb_object_t* object, const char* field);

/// What the runtime knows about an Interface. The compiler makes one for each Interface that
/// can be a type.
struct fb_interface_t {
    /// The Interface's name, NUL-terminated, for panics.
    const char* name;
};

/// An Interface that a Class's objects implement, and the Class's table for it: for each of the
/// Interface's Methods, and each Get and Set of its Properties, in order, the function that the
/// Class's objects run.
struct fb_implementation_t {
    const fb_interface_t* interface;
    void* const* methods;
};

/// What the runtime knows about a Class. The compiler makes one for each Class in the program,
/// with its functions.
struct fb_class_t {
    /// The Class's name, NUL-terminated, for the leak report.
    const char* name;
    /// An object's size in bytes, header included.
    std::int64_t size;
    /// Runs the object's Destroy(), then those of the Classes it extends. Null when none of them
    /// has one.
    void (*destroy)(fb_object_t* object);
    /// Releases the object's fields, after its destroy, or without it when New threw. A T[]'s
    /// releases its elements and frees their buffer. The runtime frees the object afterwards.
    void (*release)(fb_object_t* object);
    /// Calls `visitor` with `context` and each object the fields refer to strongly, for the
    /// cycle detector. A Weak<T> doesn't keep its object, so it isn't one.
    void (*visit)(const fb_object_t* object, fb_visitor_t visitor, void* context);
    /// How many objects of the Class exist, for the leak report.
    std::int64_t live;
    /// The Interfaces its objects implement, its own and the ones it inherits, each with the
    /// Class's table for it.
    const fb_implementation_t* interfaces;
    std::int64_t interface_count;
    /// The Class it extends, or null. Compiled code puts the Class's Method table right after
    /// this, for the calls that a Class extending it could override.
    const fb_class_t* base;
};

/// The header of a Class's object, which its fields follow. Compiled code sees the two counts as
/// one 64-bit field, which it never touches.
struct fb_object_t {
    /// How many owners the object has. 0 once it's gone, while Weak references keep its memory.
    std::int32_t refcount;
    /// How many Weak<T> values refer to it. The memory is freed once both counts are 0.
    std::int32_t weak_count;
    fb_class_t* type;
};

/// Where in the source a panic happened, as compiled code passes it to the runtime.
struct fb_location_t {
    /// The source file's path, NUL-terminated.
    const char* file;
    /// 1-based.
    std::uint32_t line;
    /// 1-based, counted in code points.
    std::uint32_t column;
};

/// The first thing a program's main calls. On Windows, it switches the console to UTF-8, keeps
/// "\n" from becoming "\r\n", and turns on colors for panics.
void fb_start();

/// Debug builds call it right after fb_start: the runtime keeps track of every object, so
/// fb_finish can point out the cycles among the ones left.
void fb_track_objects();

/// The last thing a program's main calls, after releasing the Globals. Prints a leak report to
/// stderr for Strings, and for the objects of each of the `count` Classes in `classes`, that were
/// never released, then the cycles among those objects that kept them alive, if fb_track_objects
/// ran.
void fb_finish(fb_class_t* const* classes, std::int64_t count);

/// How many Strings exist right now.
std::int64_t fb_string_live_count();

/// Adds an owner to `string`. Compiled code calls it when a String gets a new holder, like a
/// variable. Does nothing for a literal, or for null, which a String? holds for Null.
void fb_string_retain(fb_string_t* string);
/// Removes an owner from `string`, and frees it when that was the last one. Compiled code calls
/// it when a holder lets go, like a variable leaving its scope. Does nothing for a literal, or
/// for null.
void fb_string_release(fb_string_t* string);
/// A new String with `left`'s bytes, then `right`'s, for `+` on Strings. The caller owns the
/// result and must release it. The arguments are only borrowed.
fb_string_t* fb_string_concat(const fb_string_t* left, const fb_string_t* right);
/// A new String with the `count` Strings in `parts` one after another, for interpolated strings.
/// The caller owns the result and must release it. The parts are only borrowed.
fb_string_t* fb_string_join(const fb_string_t* const* parts, std::int64_t count);
/// 1 when `left` and `right` hold the same bytes, and 0 otherwise, for `==` and `!=` on Strings.
std::int32_t fb_string_equal(const fb_string_t* left, const fb_string_t* right);
/// Below 0, 0, or above 0 as `left` comes before `right`, is the same, or comes after, for `<`
/// and the rest on Strings. They compare byte by byte, which puts UTF-8 in code point order.
std::int32_t fb_string_compare(const fb_string_t* left, const fb_string_t* right);
/// A hash of a String's bytes, for Hashable.
std::uint64_t fb_string_hash(const fb_string_t* string);

/// A hash of an integer's bits, which spreads each bit over all of the result, for Hashable.
std::uint64_t fb_hash_integer(std::uint64_t bits);
/// A hash of a float, the same for 0.0 and -0.0, since they're equal.
std::uint64_t fb_hash_float(double value);
/// A hash so far combined with the next part's, as a Struct's, tuple's, or array's Hash combines
/// its parts' in order.
std::uint64_t fb_hash_combine(std::uint64_t seed, std::uint64_t hash);

/// `value` as decimal text, like "-42", for interpolated strings. The caller owns the result.
fb_string_t* fb_string_from_int(std::int64_t value);
/// `value` as decimal text, for interpolated strings. The caller owns the result.
fb_string_t* fb_string_from_uint(std::uint64_t value);
/// The shortest text that reads back as `value`, like "0.1", for interpolated strings. It
/// matches the compiler's constant folding. The caller owns the result.
fb_string_t* fb_string_from_float32(float value);
/// The shortest text that reads back as `value`, like "0.1", for interpolated strings. It
/// matches the compiler's constant folding. The caller owns the result.
fb_string_t* fb_string_from_float64(double value);
/// "True" when `value` isn't 0, and "False" otherwise, for interpolated strings. The caller owns
/// the result.
fb_string_t* fb_string_from_bool(std::int32_t value);

/// `base` to the power `exponent`, for `**` on a signed integer type that's `bits` wide (8, 16,
/// 32, or 64). Compiled code widens the operands to 64 bits, and the result comes back
/// sign-extended. When `checked` isn't 0 (debug builds), overflow panics; otherwise it wraps. A
/// negative exponent always panics. `location` is where a panic reports, and can be null.
std::int64_t fb_power_int(std::int64_t base, std::int64_t exponent, std::uint32_t bits,
                          std::int32_t checked, const fb_location_t* location);
/// `base` to the power `exponent`, for `**` on an unsigned integer type that's `bits` wide (8,
/// 16, 32, or 64). Compiled code widens the operands to 64 bits. When `checked` isn't 0 (debug
/// builds), overflow panics; otherwise it wraps. `location` is where a panic reports, and can be
/// null.
std::uint64_t fb_power_uint(std::uint64_t base, std::uint64_t exponent, std::uint32_t bits,
                            std::int32_t checked, const fb_location_t* location);

/// A new object of `type` with one owner, and every field zeroed. Compiled code sets the fields
/// next.
fb_object_t* fb_object_new(fb_class_t* type);
/// Adds an owner to `object`. Does nothing for null, which a variable of a Class holds before
/// it's assigned.
void fb_object_retain(fb_object_t* object);
/// Removes an owner from `object`. The last one runs its Class's destroy and release, then frees
/// it, unless a Weak<T> still refers to it; a Destroy() that keeps a reference to its object
/// panics. Does nothing for null.
void fb_object_release(fb_object_t* object);
/// Like fb_object_retain and fb_object_release, for objects that threads share (a Shared Class,
/// in M4), whose counts change atomically.
void fb_object_retain_atomic(fb_object_t* object);
void fb_object_release_atomic(fb_object_t* object);
/// Adds a Weak<T> that refers to `object`, which keeps its memory, not the object. Does nothing
/// for null.
void fb_object_retain_weak(fb_object_t* object);
/// Removes a Weak<T> that refers to `object`. The last one frees an object that's already gone.
/// Does nothing for null.
void fb_object_release_weak(fb_object_t* object);
/// What reading a Weak<T> gives: `object` with a new owner, which the caller releases, or null
/// once the object is gone. `object` can be null.
fb_object_t* fb_object_load_weak(fb_object_t* object);
/// Lets go of an object whose New threw: its fields are released, without its Destroy(), since
/// New may not have set what Destroy() reads. If New gave Self to something that kept it, the
/// object stays, like any other.
void fb_object_discard(fb_object_t* object);
/// A hash of which object `object` is, for a Class's built-in Hash, which goes with comparing
/// objects themselves.
std::uint64_t fb_hash_object(const fb_object_t* object);
/// Whether `object` is of `type`, or of a Class that extends it, for 'Is'. 0 for null.
std::int32_t fb_object_is(const fb_object_t* object, const fb_class_t* type);
/// Panics at `location` unless `object` is of `type`, or of a Class that extends it, for 'As':
/// "expected Boss, found Grunt", or "found Null".
void fb_object_expect(const fb_object_t* object, const fb_class_t* type,
                      const fb_location_t* location);
/// The table that `object`'s Class has for `interface`, for a call through an Interface. The
/// compiler makes sure the Class implements it.
void* const* fb_interface_methods(const fb_object_t* object, const fb_interface_t* interface);
/// Whether `object`'s Class implements `interface`, for 'Is'. 0 for null.
std::int32_t fb_object_implements(const fb_object_t* object, const fb_interface_t* interface);
/// Panics at `location` unless `object`'s Class implements `interface`, for 'As': "expected
/// Damageable, found Rock", or "found Null".
void fb_object_expect_implements(const fb_object_t* object, const fb_interface_t* interface,
                                 const fb_location_t* location);

/// A T[]: an object of a Class the compiler makes for each array type, like "Int32[]", whose
/// fields are its count, its capacity, and its elements. They live in a buffer of their own, so
/// the array can grow without the object moving. Compiled code stores, retains, and releases
/// the elements, since only it knows their type.
struct fb_array_t {
    fb_object_t header;
    std::int64_t count;
    std::int64_t capacity;
    void* data;
};

/// A new array of `type` with `count` elements of `element_size` bytes, all zero bytes, which
/// compiled code sets next.
fb_array_t* fb_array_new(fb_class_t* type, std::int64_t element_size, std::int64_t count);
/// Makes room for one more element at the end, and returns where compiled code stores it.
void* fb_array_push(fb_array_t* array, std::int64_t element_size);
/// Makes room for one more element at `index`, moving the ones from there up, and returns
/// where compiled code stores it. Panics at `location` unless `index` is from 0 to the count.
void* fb_array_insert(fb_array_t* array, std::int64_t element_size, std::int64_t index,
                      const fb_location_t* location);
/// Takes out the element at `index`, which compiled code checked and released, moving the ones
/// after it down.
void fb_array_remove(fb_array_t* array, std::int64_t element_size, std::int64_t index);
/// Empties the array, whose elements compiled code released. The buffer stays for new ones.
void fb_array_clear(fb_array_t* array);
/// A new array of the same Class, with a copy of each element's bytes. Compiled code retains
/// the references in them.
fb_array_t* fb_array_clone(const fb_array_t* array, std::int64_t element_size);
/// Frees the buffer, for an array's destroy once it released the elements. The runtime frees
/// the object itself.
void fb_array_free(const fb_array_t* array);
/// Adds the code points of `string`'s UTF-8 to `into`, an Int32[], for "s.Chars".
void fb_string_chars(const fb_string_t* string, fb_array_t* into);
/// Adds `string`'s UTF-8 bytes to `into`, a UInt8[], for "s.Bytes".
void fb_string_bytes(const fb_string_t* string, fb_array_t* into);
/// Panics at `location`: "index 5 is out of range for 3 elements".
[[noreturn]] void fb_index_out_of_range(std::int64_t index, std::int64_t count,
                                        const fb_location_t* location);

/// Writes `string` and a newline to stdout, for the built-in Print. The String is only borrowed.
void fb_print(const fb_string_t* string);

/// Prints "panic: <message> at <file>:<line>:<column>" to stderr and exits with code 101.
/// Compiled code calls it when a run-time check fails. `location` can be null, which leaves out
/// the " at ..." part.
[[noreturn]] void fb_panic(const char* message, const fb_location_t* location);
/// Like fb_panic, with a String the program made as the message, for Panic and Assert. The
/// String is only borrowed.
[[noreturn]] void fb_panic_string(const fb_string_t* message, const fb_location_t* location);
/// Prints "uncaught <Class>: <Message> at <file>:<line>:<column>" to stderr and exits with code
/// 1, for an error that top-level code let out. `error` is an object of a Class that extends
/// Error, whose first field is its Message. `location` can be null, as for fb_panic.
[[noreturn]] void fb_error_uncaught(const fb_object_t* error, const fb_location_t* location);
}
