#pragma once

#include <functional>
#include <uv.h>

// What the runtime's parts over libuv share with io.cpp: the I/O thread, for requests other
// than stdin's.
namespace fb::runtime {

/// Runs `job` on the I/O thread with its loop, starting the thread the first time, or right
/// away with nullptr once the program is ending, when the loop takes nothing more.
void on_io_thread(std::function<void(uv_loop_t*)> job);

/// Has `stop` run on the I/O thread's loop as it stops, to close the handles a part of the
/// runtime keeps open there, which would keep the loop going. At most one per part, and only
/// the I/O thread calls it.
void at_io_stop(void (*stop)(uv_loop_t* loop));

} // namespace fb::runtime
