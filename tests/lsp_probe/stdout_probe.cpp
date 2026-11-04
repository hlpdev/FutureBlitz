// What take_stdout() is for, in a process of its own: `stdout_probe <mode> [file]` gets its stderr
// ready as the mode says, takes stdout, writes "message" to what it took, and "stray" to stdout, as
// a library or a child would. outbox_tests look at where each went.
//
// With "file", stderr is the file. With "closed", there's none. With "child", stderr is the file,
// and it starts a child that lives for a while, inheriting what it can, and doesn't wait for it, so
// whatever reads what it took sees its end at once unless the child inherited it. "sleep" is that
// child.

#include "fb/lsp/outbox.hpp"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

void stderr_to(const char* path) {
#ifdef _WIN32
    const int file = _open(path, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, 0600);
    static_cast<void>(_dup2(file, 2));
    static_cast<void>(_close(file));
#else
    const int file = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    static_cast<void>(dup2(file, 2));
    static_cast<void>(close(file));
#endif
}

void close_stderr() {
#ifdef _WIN32
    static_cast<void>(_close(2));
    SetStdHandle(STD_ERROR_HANDLE, INVALID_HANDLE_VALUE);
#else
    static_cast<void>(close(2));
#endif
}

void start_child(const char* self) {
#ifdef _WIN32
    std::string command = std::string("\"") + self + "\" sleep";
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessA(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                       nullptr, &startup, &process) != 0) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
#else
    if (fork() == 0) {
        execl(self, self, "sleep", nullptr);
        _exit(1);
    }
#endif
}

void write_stray() {
    std::fputs("stray", stdout);
    static_cast<void>(std::fflush(stdout));
}

} // namespace

// NOLINTNEXTLINE(bugprone-exception-escape): a probe that throws fails its test
int main(const int count, const char* const* arguments) {
    const std::string mode = count > 1 ? arguments[1] : "";
    if (mode == "sleep") {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        return 0;
    }

    if (mode == "closed") {
        close_stderr();
    } else if (count > 2) {
        stderr_to(arguments[2]);
    }

    const int taken = fb::lsp::take_stdout();
    fb::lsp::fd_buffer_t buffer(taken);
    buffer.sputn("message", 7);
    write_stray();
    if (mode == "child") {
        start_child(arguments[0]);
    }

    return 0;
}
