#pragma once

#include "fb/runtime.hpp"

#include <cstdint>

// std.os's processes and signals in the runtime, over libuv: starting programs with their
// arguments, environment, folder, and streams, moving the bytes of their pipes, waiting for them
// to end, and counting the signals that OnInterrupt's and OnTerminate's handlers wait for. Their
// prefix, fb_os_, links libuv with them. A thread of their own runs libuv's loop for them, from
// the first request until the program ends. Strings and arrays are only borrowed.
//
// A child is a handle, which each std.os object that has it holds, through fb_os_process_hold
// and fb_os_process_release. A stream is 0 stdin, 1 stdout, or 2 stderr, and a Stdio 0 the
// program's own, 1 a pipe, or 2 none. What fails gives libuv's error, a negative number, which
// fb_os_error_kind and fb_os_error_text say more of. An exit status is the code the program
// exited with, 0 or more, or minus the signal that ended it.

extern "C" {

/// Starts `program`, looked up as libuv looks one up, on PATH, with `arguments`, a String[], in
/// `folder`, or this program's folder when it's empty, and its streams as `input`, `output`, and
/// `error` say. Its environment is this program's, or empty when `cleared` isn't 0, with each of
/// `names`, a String[], set to the String? at its place in `values`, or taken out for null, in
/// order. Gives the child's handle, or 0, with libuv's error in `failure`, or 1 there when a text
/// has a NUL, which none can, or 2 when a name is empty or has an '=', and nothing starts.
std::uint64_t fb_os_process_spawn(const fb_string_t* program, const fb_array_t* arguments,
                                  const fb_array_t* names, const fb_array_t* values,
                                  std::int32_t cleared, const fb_string_t* folder,
                                  std::int32_t input, std::int32_t output, std::int32_t error,
                                  std::int64_t* failure);
/// Another holder of the child's handle.
void fb_os_process_hold(std::uint64_t child);
/// A holder lets go of the handle. Once none holds it, its stdin closes, and the runtime lets go
/// of the child once it's ended, which it doesn't wait for: nothing kills it.
void fb_os_process_release(std::uint64_t child);
/// The OS's number for the child's process.
std::int64_t fb_os_process_pid(std::uint64_t child);
/// Waits for the child to end, and gives its exit status.
std::int64_t fb_os_process_wait(std::uint64_t child);
/// fb_os_process_wait's Async twin, whose Task<Int64> gives the status.
void fb_os_process_wait_async(fb_task_t* task, std::uint64_t child);
/// 1, with the exit status in `status`, once the child has ended, and 0 before.
std::int32_t fb_os_process_try_wait(std::uint64_t child, std::int64_t* status);
/// Ends the child at once, by SIGKILL, or on Windows TerminateProcess: 0, as for one that's
/// ended already, or libuv's error.
std::int64_t fb_os_process_kill(std::uint64_t child);
/// Reads up to `count` bytes of the child's stdout or stderr into `into`, waiting for one at
/// least: gives how many, 0 once the stream has ended, or libuv's error.
std::int64_t fb_os_process_read(std::uint64_t child, std::int32_t stream, std::uint8_t* into,
                                std::int64_t count);
/// fb_os_process_read's Async twin, whose Task<Int64> gives the count. `into` stays its caller's
/// until the Task is done.
void fb_os_process_read_async(fb_task_t* task, std::uint64_t child, std::int32_t stream,
                              std::uint8_t* into, std::int64_t count);
/// Has the child's stdout or stderr read as it comes, holding what no read has taken yet, so the
/// child never waits for room in that pipe while the program reads the other one.
void fb_os_process_drain(std::uint64_t child, std::int32_t stream);
/// Writes `count` bytes from `from` to the child's stdin, and waits until they've gone: 0, or
/// libuv's error.
std::int64_t fb_os_process_write(std::uint64_t child, const std::uint8_t* from, std::int64_t count);
/// fb_os_process_write's Async twin, whose Task<Int64> gives 0 or the error. The bytes are
/// copied, so `from` is the caller's again at once.
void fb_os_process_write_async(fb_task_t* task, std::uint64_t child, const std::uint8_t* from,
                               std::int64_t count);
/// Closes the child's stdin once what was written to it has gone, so it reads to its end. Closing
/// it again does nothing.
void fb_os_process_close_input(std::uint64_t child);

/// What kind of error libuv's `error` is, which std.os gives as an IOErrorKind: 0 NOT_FOUND,
/// 1 PERMISSION_DENIED, 2 ALREADY_EXISTS, 3 UNEXPECTED_END, 4 INTERRUPTED, or 5 OTHER.
std::int32_t fb_os_error_kind(std::int64_t error);
/// What libuv says `error` is, as "no such file or directory".
fb_string_t* fb_os_error_text(std::int64_t error);

/// Adds the program's own arguments, after its name, to `into`, a String[], as the OS gave them.
void fb_os_process_arguments(fb_array_t* into);

/// Watches for `signal`, from before it returns, instead of letting it end the program, and gives
/// how many have come before, which fb_os_signal_wait counts from. A signal is 1 an interrupt,
/// Ctrl+C, or on Windows Ctrl+Break too, or 2 a request to end, SIGTERM or SIGHUP, or on Windows
/// its console closing.
std::int64_t fb_os_signal_watch(std::int32_t signal);
/// Waits until more than `seen` of `signal` have come, and gives how many have, or 0 once the
/// program's code is done, its coroutines too, when the watching stops and the signals do what
/// they do.
std::int64_t fb_os_signal_wait(std::int32_t signal, std::int64_t seen);
}
