#pragma once

#include <functional>
#include <uv.h>

// What the runtime's parts over libuv share with io.cpp: the I/O thread, for requests other
// than stdin's.
namespace fb::runtime {

/// Runs `job` on the I/O thread with its loop, starting the thread the first time, or right
/// away with nullptr once the program is ending, when the loop takes nothing more.
void on_io_thread(std::function<void(uv_loop_t*)> job);

/// Has `stop` run on the I/O thread as the program ends, before the thread lets go of its own
/// handles: it closes those its part opened on `loop`, and answers what still waits. At most one
/// per part of the runtime, the last one given running first.
void at_io_stop(void (*stop)(uv_loop_t* loop));

} // namespace fb::runtime
