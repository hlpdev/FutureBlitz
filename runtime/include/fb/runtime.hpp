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
    /// Its name with its module first, and its type arguments' modules, like
    /// "std.core.iterator.Iterator<game.Node>", which a message shows instead when another name
    /// in it is the same. Can be null.
    const char* full_name;
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
    /// Its name with each module first, like fb_interface_t's. Can be null.
    const char* full_name;
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
    /// Not 0 for a Shared Class, whose objects threads share: their counts change atomically.
    std::int64_t shared;
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

/// The fields every error has first, from std.core's Error, after its object's header.
struct fb_error_fields_t {
    fb_string_t* message;
    fb_object_t* cause;
    /// Its Place: where it was first thrown, and nowhere while `line` is 0.
    fb_string_t* file;
    std::int32_t line;
    std::int32_t column;
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

/// The last thing a program's main calls, after fb_pool_finish and releasing the Globals. It
/// ends the main scheduler (fb_scheduler_end), for coroutines that releasing the Globals
/// started, and finishes the pool again, for jobs it started. Prints a leak report to
/// stderr for Strings, and for the objects of each of the `count` Classes in `classes`, that were
/// never released, then the cycles among those objects that kept them alive, if fb_track_objects
/// ran.
void fb_finish(fb_class_t* const* classes, std::int64_t count);

/// How many Strings exist right now.
std::int64_t fb_string_live_count();

/// A copy of `string`'s text with a NUL after it, for a call of C code that takes a CString,
/// which reads it up to its first NUL. Compiled code frees it with fb_cstring_free right after
/// the call.
char* fb_cstring_new(const fb_string_t* string);

/// Frees a copy that fb_cstring_new made. Panics for anything else, or a copy freed already.
void fb_cstring_free(char* copy);

/// Frees a copy that fb_cstring_new made, giving 1, or 0 for anything else, or a copy freed
/// already, which it leaves alone, so std's CString.Free panics where it's called.
std::int32_t fb_cstring_free_known(char* copy);

/// How many of fb_cstring_new's copies exist right now.
std::int64_t fb_cstring_live_count();

/// For std.ffi's CString.Length: how many bytes `text` has before its NUL.
std::uint64_t fb_cstring_length(const char* text);

/// For std.ffi's Mem, which goes through these rather than C's own functions, so a program can
/// declare those as it likes: `bytes` of new memory, all 0, or null when there isn't that much.
/// It's always 1 byte or more, since calloc may give null for none.
void* fb_mem_alloc(std::uint64_t bytes);
/// As fb_mem_alloc, but the memory isn't set to 0.
void* fb_mem_alloc_uninitialized(std::uint64_t bytes);
/// What fb_mem_alloc, fb_mem_alloc_uninitialized, or fb_mem_resize gave, or null for new memory,
/// in `bytes` of memory, which keeps what fits, or null when there isn't that much, which leaves
/// `memory` as it was. It's always 1 byte or more, since realloc may give memory back for none.
void* fb_mem_resize(void* memory, std::uint64_t bytes);
/// Gives back what fb_mem_alloc, fb_mem_alloc_uninitialized, or fb_mem_resize gave. Null is
/// nothing to give back.
void fb_mem_free(void* memory);
/// `bytes` of new memory, all 0, at an address that's a multiple of `alignment`, a power of two,
/// or null when there isn't that much. Only fb_mem_free_aligned gives it back, since Windows'
/// aligned memory is its own.
void* fb_mem_alloc_aligned(std::uint64_t bytes, std::uint64_t alignment);
/// Gives back what fb_mem_alloc_aligned gave. Null is nothing to give back.
void fb_mem_free_aligned(void* memory);
/// Copies `bytes` from `source` to `destination`, which can overlap. Neither is null.
void fb_mem_copy(void* destination, const void* source, std::uint64_t bytes);
/// Sets `bytes` from `destination` on to `value`. It isn't null.
void fb_mem_fill(void* destination, std::uint8_t value, std::uint64_t bytes);
/// memcmp's order of `bytes` at `a` and at `b`, which aren't null.
std::int32_t fb_mem_compare(const void* a, const void* b, std::uint64_t bytes);

/// A new String holding C's `text` up to its NUL, with U+FFFD for each byte that doesn't start
/// well-formed UTF-8, or null for a null `text`, for an Extern Function that gives a String or
/// String?.
fb_string_t* fb_string_from_cstring(const char* text);
/// A new String of `length` bytes from `text`, NULs too, with U+FFFD for each byte that doesn't
/// start well-formed UTF-8, as the runtime reads text from outside the program.
fb_string_t* fb_string_from_bytes(const char* text, std::int64_t length);

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
/// A seed for std.math's default generator on a thread that draws before SeedRandom: the OS's
/// randomness, mixed with the clock and a count of the seeds made, so each run and each thread
/// differ.
std::uint64_t fb_random_seed();

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
/// it's assigned. A Shared Class's object counts atomically, so any thread can.
void fb_object_retain(fb_object_t* object);
/// Removes an owner from `object`. The last one runs its Class's destroy and release, then frees
/// it, unless a Weak<T> still refers to it; a Destroy() that keeps a reference to its object
/// panics. Does nothing for null. A Shared Class's object is destroyed by whichever thread lets
/// go last.
void fb_object_release(fb_object_t* object);
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
/// Makes room for `capacity` elements in all, so the array can grow to that many without moving
/// them. Panics at `location` when `capacity` is negative.
void fb_array_reserve(fb_array_t* array, std::int64_t element_size, std::int64_t capacity,
                      const fb_location_t* location);
/// Gives back the room beyond the array's count.
void fb_array_trim(fb_array_t* array, std::int64_t element_size);
/// A new array of the same Class, with a copy of each element's bytes. Compiled code retains
/// the references in them.
fb_array_t* fb_array_clone(const fb_array_t* array, std::int64_t element_size);
/// A new String of a UInt8[]'s bytes, as fb_string_from_bytes() makes one, for std's code that
/// makes text a byte at a time.
fb_string_t* fb_string_from_array(const fb_array_t* bytes);
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
/// Panics at `location` for a UInt64 index, which can be past an Int64's range.
[[noreturn]] void fb_unsigned_index_out_of_range(std::uint64_t index, std::int64_t count,
                                                 const fb_location_t* location);

/// Work for another thread: a function, and what it works on.
using fb_work_t = void (*)(void* context);
/// What a range runs for each chunk: `count` indexes from `first` on, 1 or more, which go up by
/// 1 and wrap past an Int64's end.
using fb_range_body_t = void (*)(void* context, std::int64_t first, std::int64_t count);

/// Work queued on the pool, which runs it on one of its workers, or on a thread that waits.
struct fb_job_t;

/// Queues `work(context)` on the pool, and starts the pool's workers if they aren't running: one
/// per core but one, and at least one. The caller owns the job, and releases it once it's waited
/// for or no longer wanted; the work runs either way.
fb_job_t* fb_job_start(fb_work_t work, void* context);
/// 1 once the job's work has returned, and 0 before.
std::int32_t fb_job_done(const fb_job_t* job);
/// Returns once the job's work has returned. Meanwhile a worker runs the jobs it started itself,
/// which is where one it waits for usually is, so it doesn't leave the pool a worker short. It
/// runs no one else's, which could wait for it in turn.
void fb_job_wait(const fb_job_t* job);
/// Lets go of the job, whose memory goes once its work has returned too.
void fb_job_release(fb_job_t* job);
/// Calls `body` with `context` for chunks of the indexes from `first` through `last`, on the
/// pool's workers and the calling thread, and returns once every chunk is done. Each index is in
/// one chunk. The indexes go up from `first` and wrap past an Int64's end, so `last` before
/// `first` is a range through that end, as an unsigned one can be: there are 1 to 2^64 of them.
/// The caller decides whether a range has any.
void fb_parallel_for(std::int64_t first, std::int64_t last, fb_range_body_t body, void* context);
/// For a Parallel For: like fb_parallel_for, with the code in `body`'s Class's Method table's
/// first entry, which takes the object and a chunk's first index and count.
void fb_parallel_closure(std::int64_t first, std::int64_t last, fb_object_t* body);
/// How many workers the pool has, or will once it starts.
std::int64_t fb_pool_size();
/// main calls it when the top-level code ends, before releasing the Globals: runs the jobs left,
/// waits for the running ones, and stops the workers. A job started afterwards starts them again.
void fb_pool_finish();

/// What releases a ThreadLocal Global's value on the thread that runs it.
using fb_release_t = void (*)();
/// A thread has made its own value of a ThreadLocal Global, which `release` lets go of as the
/// thread ends: a pool worker's as the pool stops, a thread of its own's after its job, the main
/// thread's in fb_thread_locals_release, and any other thread's as it exits.
void fb_thread_local_added(fb_release_t release);
/// The calling thread begins running a ThreadLocal Global's initializer, or is done with it.
/// Meanwhile a pool worker's waits run only the jobs it starts after this, since an older one
/// could use the Global it's making.
void fb_thread_local_making();
void fb_thread_local_made();
/// Releases the calling thread's ThreadLocal Globals, the last made first, and any that a
/// Destroy makes meanwhile. main calls it after fb_pool_finish, before releasing the Globals,
/// which their Destroys can use, and fb_finish again, for ones the Globals' Destroys made.
void fb_thread_locals_release();

/// A Mutex<T>'s lock, which its value follows. Zero is a Mutex that no thread holds.
struct fb_mutex_t {
    /// 0 when no thread holds it, 1 when one does, and 2 when one does and others may wait.
    std::int32_t state;
    /// The number of the thread holding it, or 0. Each thread gets its own the first time it
    /// takes a Mutex.
    std::uint32_t owner;
};

/// Takes `mutex`, waiting while another thread holds it, for a Lock block. Panics at `location`
/// when this thread holds it already, since it would wait forever.
void fb_mutex_lock(fb_mutex_t* mutex, const fb_location_t* location);
/// Lets go of `mutex`, at the end of a Lock block, and wakes a thread waiting for it.
void fb_mutex_unlock(fb_mutex_t* mutex);

/// Like fb_job_start, on a thread of its own, for long, blocking work that would keep a worker
/// from the pool. fb_pool_finish waits for it too.
fb_job_t* fb_job_start_thread(fb_work_t work, void* context);

/// A Channel<T>'s object: a ring of `capacity` elements of `element_size` bytes, which threads
/// send to and receive from, behind a lock. Compiled code copies an element's bytes in and out,
/// with its references.
struct fb_channel_t {
    fb_object_t header;
    std::int64_t element_size;
    std::int64_t capacity;
    /// Where the oldest element is, and how many there are.
    std::int64_t head;
    std::int64_t count;
    /// Not 0 once it's closed: no more can be sent, and receiving takes what's left.
    std::int64_t closed;
    unsigned char* data;
    /// The runtime's lock.
    void* lock;
};

/// A new Channel of `type`, a Class compiled code makes for each Channel type, with room for
/// `capacity` elements. Panics at `location` unless that's 1 or more.
fb_channel_t* fb_channel_new(fb_class_t* type, std::int64_t element_size, std::int64_t capacity,
                             const fb_location_t* location);
/// Copies `element` in, waiting while the Channel is full, which runs no jobs, since one could
/// wait on the Channel in turn. The Channel takes the element's references. Panics at `location`
/// once it's closed.
void fb_channel_send(fb_channel_t* channel, const void* element, const fb_location_t* location);
/// Copies `element` in, unless the Channel is full or closed: 1 when it did, and 0 when it
/// didn't, and the caller still has the element's references.
std::int32_t fb_channel_try_send(fb_channel_t* channel, const void* element);
/// Copies the oldest element out to `element`, with its references, waiting while the Channel is
/// empty, which runs no jobs: 1 when it did, and 0 once it's closed and empty.
std::int32_t fb_channel_receive(fb_channel_t* channel, void* element);
/// Like fb_channel_receive, without waiting: 0 when nothing is there.
std::int32_t fb_channel_try_receive(fb_channel_t* channel, void* element);
/// Closes the Channel, which wakes the threads waiting on it.
void fb_channel_close(fb_channel_t* channel);
/// For a Channel's Class's release: takes out the oldest element left, whose references compiled
/// code releases, or gives null when none are left.
void* fb_channel_take_left(fb_channel_t* channel);
/// The element `index` after the oldest, or null past the last, for a Channel's Class's visit,
/// which the leak report calls once no thread uses it.
void* fb_channel_element(const fb_channel_t* channel, std::int64_t index);
/// For a Channel's Class's release, once the elements are released: frees the ring and the lock.
void fb_channel_free(const fb_channel_t* channel);

/// A Task's or Thread's object: the header, the job running its work, and the closure that does
/// it, until it runs. A Task's result follows. Compiled code makes a Class for each Task type,
/// whose Method table's first entry runs the closure and keeps the result. Threads share the
/// object, so the Class is shared.
struct fb_task_t {
    fb_object_t header;
    fb_job_t* job;
    fb_object_t* closure;
};

/// Starts a Task's work on the pool. The job holds a reference to the task until it's done.
void fb_task_start(fb_task_t* task);
/// Starts a Thread's work on a thread of its own.
void fb_task_start_thread(fb_task_t* task);
/// Returns once the task's work is done, for Wait and Join. Meanwhile a worker runs the jobs it
/// started itself, as fb_job_wait does, and the main thread ticks the main scheduler.
void fb_task_wait(const fb_task_t* task);
/// 1 once the task's work is done, and 0 before, for IsDone.
std::int32_t fb_task_done(const fb_task_t* task);
/// Gives an Async Function's new Task its completion, which fb_task_finish sets.
void fb_task_begin(fb_task_t* task);
/// The Async Function is done: what waits for its Task can go on.
void fb_task_finish(const fb_task_t* task);
/// The scheduler running the code resumes `coroutine`, which is `own`'s Async Function's, once
/// `task` is done. Coroutines run on the main thread, which runs the main scheduler once
/// top-level code ends, and whenever it waits for a Task; a Scheduler's run when it's ticked.
void fb_await(void* coroutine, fb_task_t* own, fb_task_t* task);
/// For NextFrame: the scheduler running the code finishes `task`, which fb_task_begin set up, at
/// its next tick.
void fb_wait_frame(fb_task_t* task);
/// For Wait and RealWait: the scheduler running the code finishes `task` once `nanoseconds` of
/// its time have passed, or of the real clock's when `real` isn't 0. The main scheduler's time is
/// the real clock's.
void fb_wait_time(fb_task_t* task, std::int64_t nanoseconds, std::int32_t real);

/// A Scheduler's object: what the runtime keeps for it, which runs its coroutines when it's
/// ticked.
struct fb_scheduler_t {
    fb_object_t header;
    void* state;
};

/// A new Scheduler of `type`, a Class compiled code makes for it.
fb_scheduler_t* fb_scheduler_new(fb_class_t* type);
/// Moves the coroutine of `task`'s Async Function, with the coroutines and timers it waits for,
/// from the scheduler running the code to the Scheduler, which runs it from then on. A Task
/// that's done, or that isn't an Async Function's, has nothing to move.
void fb_scheduler_run(const fb_scheduler_t* scheduler, const fb_task_t* task);
/// Moves the Scheduler's time on by `seconds`, then resumes its coroutines that can go on.
void fb_scheduler_tick(const fb_scheduler_t* scheduler, double seconds);
/// 1 when no coroutine waits on the Scheduler, and 0 otherwise.
std::int32_t fb_scheduler_idle(const fb_scheduler_t* scheduler);
/// For a Scheduler's Class's release: lets go of what it holds. A coroutine still waiting on it
/// is destroyed, which lets go of what that holds, and its Task is never done.
void fb_scheduler_free(const fb_scheduler_t* scheduler);
/// Memory for an Async Function's coroutine, and back.
void* fb_coroutine_alloc(std::int64_t size);
void fb_coroutine_free(void* frame);
/// main calls it when the top-level code ends, before fb_pool_finish: runs the coroutines until
/// none is waiting, or none can go on, since what they wait for is on a Scheduler that nothing
/// ticks any more, and then ends the main scheduler, as fb_scheduler_end does.
void fb_scheduler_finish();
/// Ends the main scheduler, as fb_scheduler_free does a Scheduler: the coroutines still waiting
/// on it are destroyed, and their Tasks, and the timers', are never done. fb_finish calls it, for
/// coroutines that releasing the Globals started.
void fb_scheduler_end();
/// Lets go of the task's job, for its Class's release.
void fb_task_release(const fb_task_t* task);

/// Writes `texts`, a String[], with a space between each two, to stdout, or to stderr when
/// `to_error` isn't 0, then a line break when `line` isn't 0, for std.core's Print, Write,
/// PrintError, and WriteError. What goes to stdout is flushed before anything goes to stderr, so
/// the two show what the program wrote in its order. The array is only borrowed. Writes from
/// different threads don't mix.
void fb_write(const fb_array_t* texts, std::int32_t to_error, std::int32_t line);
/// Flushes what's been written to stdout and stderr.
void fb_flush();

// The fb_io_ functions go through the I/O thread, which runs libuv's loop. It starts with the
// first, and stops as the program ends. A program links libuv only when it calls one.

/// Writes `prompt` to stdout, flushed, then reads a line from stdin, the next of what the I/O
/// thread reads: a new String of it, without its line break ("\n" or "\r\n"), with U+FFFD for
/// each byte that doesn't start well-formed UTF-8, or null at the end of input. It blocks,
/// running nothing else meanwhile.
fb_string_t* fb_io_input(const fb_string_t* prompt);
/// Writes `prompt`, then finishes `task`, a new Task<String?>, with the line that fb_io_input
/// would give, once the I/O thread has it. A Task's result is right after its fb_task_t.
void fb_io_input_async(fb_task_t* task, const fb_string_t* prompt);

/// Looks up `host`'s addresses, as getaddrinfo finds them, into `output`, a UInt8[], each as 17
/// bytes: 4 or 6 for its family, then its 16 bytes, an IPv4 address's 4 first. Gives 0, or
/// libuv's error, which fb_io_error_text says in English. It blocks, as fb_io_input does.
std::int32_t fb_io_resolve(const fb_string_t* host, fb_array_t* output);
/// The same, finishing `task`, a new Task<UInt8[]>, once the I/O thread has the addresses, with a
/// new array of `like`'s Class: fb_io_resolve's result as 4 bytes, the least significant first,
/// then the records. It reads only `like`'s Class, so the caller can let go of it whenever.
void fb_io_resolve_async(fb_task_t* task, const fb_string_t* host, const fb_array_t* like);
/// What libuv's error `code` means.
fb_string_t* fb_io_error_text(std::int32_t code);

/// Prints "panic: <message> at <file>:<line>:<column>" to stderr and exits with code 101.
/// Compiled code calls it when a run-time check fails. `location` can be null, which leaves out
/// the " at ..." part. On any thread, it ends the whole program, at once: a second thread that
/// panics meanwhile waits for the end, so only the first one's message shows.
[[noreturn]] void fb_panic(const char* message, const fb_location_t* location);
/// Like fb_panic, with a String the program made as the message, for Panic and Assert. The
/// String is only borrowed.
[[noreturn]] void fb_panic_string(const fb_string_t* message, const fb_location_t* location);
/// Sets a thrown error's Place to `location`, unless an earlier Throw set it. `error` is an
/// object of a Class that extends Error, whose first fields are fb_error_fields_t's.
void fb_error_thrown(fb_object_t* error, const fb_location_t* location);
/// Prints "uncaught <Class>: <Message> at <file>:<line>:<column>" to stderr, at the error's
/// Place, or else at `location`, where top-level code let it out, which can be null, then a
/// "caused by" line like it for each Cause, and exits with code 1. `error` is an object of a
/// Class that extends Error.
[[noreturn]] void fb_error_uncaught(const fb_object_t* error, const fb_location_t* location);
}
