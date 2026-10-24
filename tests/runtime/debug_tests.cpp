#include "fb/debug.hpp"

#include <catch2/catch_test_macros.hpp>

#ifndef _WIN32
#include <csignal>
#include <cstdint>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

TEST_CASE("no debugger is attached to the tests, so a breakpoint goes on", "[runtime][debug]") {
    CHECK(fb_debug_attached() == 0);
    // Without a debugger, the trap would end the process: it's left out.
    fb_debug_break();
    fb_debug_break();
    CHECK(fb_debug_attached() == 0);
}

#ifndef _WIN32

namespace {

/// What a child traced by this process did: how many traps stopped it, and how it ended.
struct traced_t {
    int traps = 0;
    int status = 0;
};

/// Continues the stopped child, giving it `signal`, or none for 0.
void resume(const pid_t child, const int signal) {
#ifdef __APPLE__
    ptrace(PT_CONTINUE, child, reinterpret_cast<caddr_t>(1), signal);
#else
    ptrace(PTRACE_CONT, child, nullptr,
           reinterpret_cast<void*>(static_cast<std::intptr_t>(signal)));
#endif
}

/// Runs a child traced by this process, which checks it's attached and calls two breakpoints.
/// The tracer hands each trap back when `passing`, as strace does, or swallows it, as gdb does.
traced_t traced(const bool passing) {
    const pid_t child = fork();
    if (child == 0) {
#ifdef __APPLE__
        ptrace(PT_TRACE_ME, 0, nullptr, 0);
#else
        ptrace(PTRACE_TRACEME, 0, nullptr, nullptr);
#endif
        std::raise(SIGSTOP);
        if (fb_debug_attached() != 1) {
            _exit(2);
        }

        fb_debug_break();
        fb_debug_break();
        _exit(0);
    }

    traced_t result;
    while (waitpid(child, &result.status, 0) == child && WIFSTOPPED(result.status)) {
        const int signal = WSTOPSIG(result.status);
        if (signal == SIGTRAP) {
            ++result.traps;
        }

        resume(child, signal == SIGTRAP && passing ? SIGTRAP : 0);
    }

    return result;
}

} // namespace

TEST_CASE("a traced program sees its tracer, and a breakpoint stops in it and goes on",
          "[runtime][debug]") {
    for (const bool passing : { false, true }) {
        INFO((passing ? "the tracer hands the trap back" : "the tracer swallows the trap"));
        const traced_t run = traced(passing);
        REQUIRE(WIFEXITED(run.status));
        CHECK(WEXITSTATUS(run.status) == 0);
        CHECK(run.traps == 2);
    }
}

#endif
