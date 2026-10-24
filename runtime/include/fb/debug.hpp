#pragma once

#include <cstdint>

// std.debug's functions in the runtime, which ask the OS about a debugger, each in its own way.

extern "C" {

/// 1 when a debugger is attached to the program, and 0 otherwise: Windows' IsDebuggerPresent,
/// macOS's P_TRACED, and Linux's TracerPid. 0 on an OS it can't ask.
std::int32_t fb_debug_attached();
/// Stops in the debugger that's attached, as a breakpoint there would, and does nothing when
/// none is, since the trap would end the program.
void fb_debug_break();
}
