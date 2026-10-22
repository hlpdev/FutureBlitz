#pragma once

#include "fb/runtime.hpp"

#include <cstdio>

// What the runtime's files share that compiled code doesn't call. io.cpp uses it without the
// rest calling io.cpp, so a program that does no I/O links none of libuv.
namespace fb::runtime {

/// A request the I/O thread starts on, whose Task a coroutine may be waiting for, so the main
/// scheduler doesn't take it as stuck meanwhile.
void start_request();

/// A request is done, its Task finished: the threads that wait look again, now that one fewer
/// is under way.
void finish_request();

/// Whether the I/O thread has requests under way.
[[nodiscard]] bool requests_pending();

/// Has `stop` run as the program ends, before the leak report, as the I/O thread does to stop.
/// At most one per part of the runtime.
void at_finish(void (*stop)());

/// Runs what at_finish() was given, the last one first.
void run_finish();

/// Writes a String's bytes, NULs too.
void write_string(const fb_string_t* string, std::FILE* stream);

/// Writes `prompt` to stdout, flushed, under the lock that keeps writes from mixing.
void write_prompt(const fb_string_t* prompt);

} // namespace fb::runtime
