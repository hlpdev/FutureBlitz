// std.debug's breakpoints: whether a debugger is attached, as each OS tells it, and a trap that
// stops in it.

#include "fb/debug.hpp"

#include <cstdint>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elifdef __APPLE__
#include <array>
#include <csignal>
#include <cstddef>
#include <mutex>
#include <signal.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>
#elifdef __linux__
#include <csignal>
#include <fstream>
#include <mutex>
#include <signal.h>
#include <string>
#include <string_view>
#endif

#if defined(__APPLE__) || defined(__linux__)
namespace {

/// What a breakpoint's trap does once a tracer hands it back: nothing.
void on_trap(int /*signal*/) {}

} // namespace
#endif

extern "C" {

std::int32_t fb_debug_attached() {
#ifdef _WIN32
    return IsDebuggerPresent() != 0 ? 1 : 0;
#elifdef __APPLE__
    // A process a debugger traces has P_TRACED among its flags, as Apple's QA1361 asks it.
    std::array name{ CTL_KERN, KERN_PROC, KERN_PROC_PID, static_cast<int>(getpid()) };
    kinfo_proc info{};
    std::size_t size = sizeof(info);
    if (sysctl(name.data(), static_cast<unsigned>(name.size()), &info, &size, nullptr, 0) != 0) {
        return 0;
    }

    return (info.kp_proc.p_flag & P_TRACED) != 0 ? 1 : 0;
#elifdef __linux__
    // The process tracing this one, as /proc/self/status says, or 0 for none.
    std::ifstream status("/proc/self/status");
    for (std::string line; std::getline(status, line);) {
        if (line.starts_with("TracerPid:")) {
            return line.find_first_not_of(" \t0", std::string_view("TracerPid:").size()) !=
                           std::string::npos
                       ? 1
                       : 0;
        }
    }

    return 0;
#else
    return 0;
#endif
}

void fb_debug_break() {
    if (fb_debug_attached() == 0) {
        return;
    }

#ifdef _WIN32
    DebugBreak();
#elif defined(__APPLE__) || defined(__linux__)
    // The tracer stops at the trap first. One that hands it back, as strace does, would have its
    // default action end the program, so an empty handler takes it while it's raised.
    static std::mutex raising;
    const std::scoped_lock lock(raising);
    struct sigaction ignoring{};
    ignoring.sa_handler = on_trap;
    sigemptyset(&ignoring.sa_mask);
    struct sigaction before{};
    sigaction(SIGTRAP, &ignoring, &before);
    std::raise(SIGTRAP);
    sigaction(SIGTRAP, &before, nullptr);
#endif
}
}
