#include "fb/debug.hpp"

#include <catch2/catch_test_macros.hpp>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <array>
#include <string>
#include <string_view>
#include <windows.h>
#else
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

#ifdef _WIN32

namespace {

constexpr std::string_view debugged_name = "a breakpoint under the test's debugger stops in it";

} // namespace

// The program the next test debugs, which runs only when it's named, as no tag of the others
// picks it.
TEST_CASE("a breakpoint under the test's debugger stops in it", "[.][debugged]") {
    CHECK(fb_debug_attached() == 1);
    fb_debug_break();
    fb_debug_break();
}

TEST_CASE("a debugged program sees its debugger, and a breakpoint stops in it and goes on",
          "[runtime][debug]") {
    // This test program again, running only the test before, as a debugger runs it.
    std::array<wchar_t, 32768> self{};
    REQUIRE(GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size())) != 0);
    std::wstring command = L"\"" + std::wstring(self.data()) + L"\" \"" +
                           std::wstring(debugged_name.begin(), debugged_name.end()) + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    REQUIRE(CreateProcessW(self.data(), command.data(), nullptr, nullptr, FALSE,
                           DEBUG_ONLY_THIS_PROCESS, nullptr, nullptr, &startup, &process) != 0);

    int breakpoints = 0;
    DWORD code = 1;
    for (bool running = true; running;) {
        DEBUG_EVENT event{};
        REQUIRE(WaitForDebugEvent(&event, INFINITE) != 0);
        DWORD next = DBG_CONTINUE;
        switch (event.dwDebugEventCode) {
        case CREATE_PROCESS_DEBUG_EVENT:
            if (event.u.CreateProcessInfo.hFile != nullptr) {
                CloseHandle(event.u.CreateProcessInfo.hFile);
            }
            break;
        case LOAD_DLL_DEBUG_EVENT:
            if (event.u.LoadDll.hFile != nullptr) {
                CloseHandle(event.u.LoadDll.hFile);
            }
            break;
        case EXCEPTION_DEBUG_EVENT:
            if (event.u.Exception.ExceptionRecord.ExceptionCode == EXCEPTION_BREAKPOINT) {
                ++breakpoints;
            } else {
                next = DBG_EXCEPTION_NOT_HANDLED;
            }
            break;
        case EXIT_PROCESS_DEBUG_EVENT:
            code = event.u.ExitProcess.dwExitCode;
            running = false;
            break;
        default:
            break;
        }

        ContinueDebugEvent(event.dwProcessId, event.dwThreadId, next);
    }

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CHECK(code == 0);
    // The loader's own, which stops a debugged program as it starts, then the program's two.
    CHECK(breakpoints == 3);
}

#else

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
