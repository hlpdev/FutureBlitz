#include "fb/process.hpp"

#include <array>
#include <cstdio>
#include <format>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace fb {

#ifdef _WIN32

namespace {

/// UTF-8 to UTF-16, for Windows' wide APIs.
std::wstring widen(const std::string_view text) {
    if (text.empty()) {
        return {};
    }

    const int size =
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
    return wide;
}

/// Quotes an argument the way CommandLineToArgvW splits it back apart.
std::wstring quote(const std::wstring& argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\"") == std::wstring::npos) {
        return argument;
    }

    std::wstring quoted = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t c : argument) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }

        // Backslashes are only special before a quote.
        quoted.append(c == L'"' ? (backslashes * 2) + 1 : backslashes, L'\\');
        backslashes = 0;
        quoted += c;
    }

    quoted.append(backslashes * 2, L'\\');
    return quoted + L'"';
}

/// The command line CreateProcessW takes: the program, then the arguments, each quoted.
std::wstring command_line(const std::filesystem::path& program,
                          const std::span<const std::string> arguments) {
    std::wstring command = quote(program.wstring());
    for (const std::string& argument : arguments) {
        command += L' ';
        command += quote(widen(argument));
    }

    return command;
}

std::string last_error() {
    return std::format("Windows error {}", GetLastError());
}

} // namespace

std::expected<process_result_t, std::string> run_process(const std::filesystem::path& program,
                                                         std::span<const std::string> arguments) {
    std::wstring command = command_line(program, arguments);
    SECURITY_ATTRIBUTES security{
        .nLength = sizeof(SECURITY_ATTRIBUTES),
        .lpSecurityDescriptor = nullptr,
        .bInheritHandle = TRUE,
    };
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (CreatePipe(&read_end, &write_end, &security, 0) == 0) {
        return std::unexpected(last_error());
    }

    // The child only gets the write end.
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    // Stdout and stderr share the pipe, so the output keeps the order it was written in.
    startup.hStdOutput = write_end;
    startup.hStdError = write_end;
    PROCESS_INFORMATION process{};
    const BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    CloseHandle(write_end); // the child has its own copy now
    if (started == 0) {
        CloseHandle(read_end);
        return std::unexpected(std::format("couldn't run {}: {}", program.string(), last_error()));
    }

    process_result_t result;
    std::array<char, 4096> buffer{};
    DWORD count = 0;
    // Read everything before waiting: a child with lots of output would block on a full pipe.
    while (ReadFile(read_end, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr) !=
               0 &&
           count > 0) {
        result.output.append(buffer.data(), count);
    }

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(process.hProcess, &exit_code);
    result.exit_code = static_cast<int>(exit_code);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    CloseHandle(read_end);
    return result;
}

std::expected<int, std::string> run_attached(const std::filesystem::path& program,
                                             std::span<const std::string> arguments) {
    std::wstring command = command_line(program, arguments);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    // The child only gets handles that can be inherited.
    for (HANDLE handle : { startup.hStdInput, startup.hStdOutput, startup.hStdError }) {
        SetHandleInformation(handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    }

    // What this process printed so far comes before the child's output.
    std::fflush(nullptr);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                       &startup, &process) == 0) {
        return std::unexpected(std::format("couldn't run {}: {}", program.string(), last_error()));
    }

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    return static_cast<int>(exit_code);
}

#else

namespace {

/// Starts a program, which isn't looked up on PATH; `actions` can be null.
std::expected<pid_t, std::string> spawn(const std::filesystem::path& program,
                                        const std::span<const std::string> arguments,
                                        const posix_spawn_file_actions_t* actions) {
    // posix_spawn takes non-const strings, so it gets copies.
    std::string path = program.string();
    std::vector<std::string> copies(arguments.begin(), arguments.end());
    std::vector<char*> argv{ path.data() };
    for (std::string& argument : copies) {
        argv.push_back(argument.data());
    }

    argv.push_back(nullptr);
    pid_t pid = 0;
    const int error = posix_spawn(&pid, path.c_str(), actions, nullptr, argv.data(), environ);
    if (error != 0) {
        return std::unexpected(std::format("couldn't run {}: {}", path, std::strerror(error)));
    }

    return pid;
}

/// The exit code, or 128 plus the signal number when a signal ended it, the way shells report.
int wait_for(const pid_t pid) {
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

} // namespace

std::expected<process_result_t, std::string> run_process(const std::filesystem::path& program,
                                                         std::span<const std::string> arguments) {
    std::array<int, 2> pipe_ends{};
    if (pipe(pipe_ends.data()) != 0) {
        return std::unexpected(std::strerror(errno));
    }

    // Stdout and stderr share the pipe, so the output keeps the order it was written in. The
    // child closes its original pipe descriptors and keeps just those two.
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipe_ends[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipe_ends[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipe_ends[0]);
    posix_spawn_file_actions_addclose(&actions, pipe_ends[1]);

    const std::expected<pid_t, std::string> pid = spawn(program, arguments, &actions);
    posix_spawn_file_actions_destroy(&actions);
    // Reading only ends once every copy of the write end is closed, and the child has its own.
    close(pipe_ends[1]);
    if (!pid) {
        close(pipe_ends[0]);
        return std::unexpected(pid.error());
    }

    process_result_t result;
    std::array<char, 4096> buffer{};
    ssize_t count = 0;
    // Read everything before waiting: a child with lots of output would block on a full pipe.
    while ((count = read(pipe_ends[0], buffer.data(), buffer.size())) > 0) {
        result.output.append(buffer.data(), static_cast<std::size_t>(count));
    }

    close(pipe_ends[0]);
    result.exit_code = wait_for(*pid);
    return result;
}

std::expected<int, std::string> run_attached(const std::filesystem::path& program,
                                             std::span<const std::string> arguments) {
    // What this process printed so far comes before the child's output.
    std::fflush(nullptr);
    return spawn(program, arguments, nullptr).transform(wait_for);
}

#endif

} // namespace fb
