#pragma once

#include "fb/runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// What the runtime's files share that compiled code doesn't call. io.cpp uses it without the
// rest calling io.cpp, so a program that does no I/O links none of libuv.
namespace fb::runtime {

/// A request the I/O thread starts on, whose Task a coroutine may be waiting for, so the main
/// scheduler doesn't take that coroutine as stuck meanwhile.
void start_request(const fb_task_t* task);

/// A request is done, its Task finished: the threads that wait look again, now that it's no
/// longer under way.
void finish_request(const fb_task_t* task);

/// The Tasks of the requests the I/O thread has under way, which it finishes in time.
[[nodiscard]] std::vector<const fb_task_t*> requested_tasks();

/// A TaskSource holds `task` until it's given, or lets go of it, which a handler of a signal may
/// do: while a thread waits for a signal, the main scheduler takes a coroutine waiting for one as
/// waiting for the signal, not as stuck.
void start_source_task(const fb_task_t* task);
void finish_source_task(const fb_task_t* task);

/// The Tasks that TaskSources hold until they're given.
[[nodiscard]] std::vector<const fb_task_t*> source_tasks();

/// `count` more threads wait for a signal, or fewer for a negative one: the pool doesn't count
/// them as work that could finish a Task, but for a TaskSource's.
void add_signal_waits(std::int64_t count);

/// Releases the calling thread's ThreadLocal Globals, as fb_thread_locals_release does, as a
/// thread the runtime started ends.
void release_thread_locals();

/// Whether the calling thread has released its ThreadLocal Globals, as the main thread does once
/// the top-level code and its jobs are done. A job it ran after would use its released ones.
[[nodiscard]] bool released_thread_locals();

/// Sets up the calling thread's state for letting go of objects, if it isn't yet, so that it
/// outlives what's set up after it: a thread destroys its thread_locals the last made first,
/// and its ThreadLocal Globals' Destroys, which a thread C started runs as it exits, let go.
void ready_letting_go();

/// Has `done` run on the main thread once the program's code is done, its coroutines too,
/// before the pool finishes its jobs and the Globals are let go of, as the signals' handlers stop
/// there, or at once on the calling thread when the code is done already. It can run twice, for
/// a call as the code ends, so the second time it must do nothing. At most one per part of the
/// runtime.
void at_code_end(void (*done)());

/// Runs what at_code_end() was given, the last one first.
void run_code_end();

/// Has `stop` run as the program ends, before the leak report, as the I/O thread does to stop.
/// At most one per part of the runtime.
void at_finish(void (*stop)());

/// Runs what at_finish() was given, the last one first.
void run_finish();

/// Has `restore` run as the program ends however it ends: a panic, an uncaught error, Ctrl+C or
/// Ctrl+Break, or a signal like SIGTERM too, to put back what the program changed that others
/// share, like stdin's flags. It mustn't wait for anything or stop a thread, and calls only what
/// a signal handler can. At most one per part of the runtime.
void at_end(void (*restore)());

/// Runs what at_end() was given, the last one first: at a normal end once nothing more is
/// printed, at a panic's or an uncaught error's before the process goes, and as the console or
/// a signal ends the program.
void run_end();

/// Has `leave` run just before a panic's or an uncaught error's message shows, after what the
/// program printed, as std.term puts the terminal back, so the message shows on the screen the
/// program started on. It calls only what a signal handler can. At most one.
void before_report(void (*leave)());

/// Runs what before_report() was given, if anything.
void run_before_report();

/// Has `watch` run on the thread that panics, after the panic's message shows and before the
/// program ends, with the message and where it is, which can be null, as fb test's runner notes
/// which test panicked. At most one.
void at_panic(void (*watch)(std::string_view message, const fb_location_t* location));

/// The calling thread's objects belong to `owner` from now on, or to no one for 0, while debug
/// builds keep track of every object, so a leak can say which test made it.
void set_object_owner(std::int64_t owner);

/// An object that's left, and the owner the thread that made it had.
struct owned_object_t {
    std::int64_t owner;
    const fb_class_t* type;
};

/// The objects left that have owners, in the order they were made.
[[nodiscard]] std::vector<owned_object_t> owned_objects();

/// Has `check` run in fb_finish, once the program's code is done, its Globals are let go of, and
/// what at_finish() was given has run, just before the leak report. At most one.
void at_leak_check(void (*check)());

/// How a message that names `type` among `others` shows it: by its name, or by its full name
/// when it's alike one of the others, as sema decides alike types.
[[nodiscard]] const char* shown_class_name(const fb_class_t* type,
                                           std::span<const fb_class_t* const> others);

/// Writes a String's bytes, NULs too.
void write_string(const fb_string_t* string, std::FILE* stream);

/// Writes `prompt` to stdout, flushed, under the lock that keeps writes from mixing.
void write_prompt(const fb_string_t* prompt);

/// A String's bytes.
[[nodiscard]] std::string_view text_of(const fb_string_t* string);

/// A new String of a copy of `text`, which is UTF-8, with one owner. Panics past the most a
/// String holds, 2147483647 bytes.
[[nodiscard]] fb_string_t* new_string(std::string_view text);

/// A code point in UTF-8, and how many bytes it takes.
struct decoded_t {
    std::uint32_t point;
    std::size_t length;
};

/// The code point that starts at `at` in `text`, or U+FFFD and 1 for a byte that doesn't start a
/// well-formed one.
[[nodiscard]] decoded_t decode_utf8(std::string_view text, std::size_t at);

/// `point`, a Unicode scalar value, in UTF-8.
[[nodiscard]] std::string encode_utf8(std::uint32_t point);

/// An environment variable's value as UTF-8, or nullopt when it isn't set. Windows's getenv gives
/// the ANSI code page's bytes, so there it's read as UTF-16.
[[nodiscard]] std::optional<std::string> environment(const char* name);

/// Whether stderr is a terminal that shows colors, and NO_COLOR isn't set, so panics and std.log's
/// lines are colored.
[[nodiscard]] bool stderr_colors();

/// A frame of a thread's stack, as the platform's unwinder finds it: where its code is, a return
/// address but for the first, and where its function starts.
struct stack_frame_t {
    std::uintptr_t address;
    std::uintptr_t start;
};

/// A thread's stack, innermost first, and whether it went on past the frames walked.
struct captured_stack_t {
    std::vector<stack_frame_t> frames;
    bool cut = false;
};

/// The calling thread's stack, from capture_stack()'s own frame outward, up to a limit.
[[nodiscard]] captured_stack_t capture_stack();

/// Writes the stack trace of `stack`, a line for each place of the program's code in it, which
/// the binaries' trace tables name, "    at <function> (<file>:<line>)", innermost first, with
/// the calls it's inlined into after it, up to a limit, then a line saying there are more.
/// Frames of other code, like the runtime's, are left out.
void write_trace(const captured_stack_t& stack, std::FILE* stream);

/// Keeps the calling thread's stack as a thrown error's trace, the frames of the program's code
/// alone, until fb_error_released().
void keep_error_trace(const fb_object_t* error);

/// The stack that keep_error_trace() kept for an error, or none.
[[nodiscard]] captured_stack_t error_trace(const fb_object_t* error);

} // namespace fb::runtime
