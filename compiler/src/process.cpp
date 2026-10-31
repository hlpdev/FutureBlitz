#include "fb/process.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
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
#include <csignal>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

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

/// This process's environment, with `environment`'s variables in place of those of their names,
/// which Windows compares without letter case, as CreateProcessW takes it: each "name=value" and
/// a NUL, then another NUL.
std::wstring environment_block(const environment_t environment) {
    std::vector<std::wstring> entries;
    std::vector<std::wstring> names;
    for (const auto& [name, value] : environment) {
        names.push_back(widen(name));
        entries.push_back(widen(name) + L"=" + widen(value));
    }

    wchar_t* strings = GetEnvironmentStringsW();
    for (const wchar_t* at = strings; at != nullptr && *at != L'\0';
         at += std::wstring_view(at).size() + 1) {
        // A name like "=C:", a drive's folder, starts with its '='.
        const std::wstring_view entry(at);
        const std::wstring_view name = entry.substr(0, entry.find(L'=', 1));
        const bool replaced = std::ranges::any_of(names, [&](const std::wstring& other) {
            return CompareStringOrdinal(name.data(), static_cast<int>(name.size()), other.data(),
                                        static_cast<int>(other.size()), TRUE) == CSTR_EQUAL;
        });
        if (!replaced) {
            entries.emplace_back(entry);
        }
    }

    if (strings != nullptr) {
        FreeEnvironmentStringsW(strings);
    }

    std::wstring block;
    for (const std::wstring& entry : entries) {
        block += entry;
        block += L'\0';
    }

    block += L'\0';
    return block;
}

/// Swallows Ctrl+C and Ctrl+Break, which the program run in this terminal gets too. A handler,
/// unlike ignoring them outright, isn't something the program inherits.
BOOL WINAPI ignore_interrupt(const DWORD event) {
    return event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT ? TRUE : FALSE;
}

} // namespace

std::expected<process_result_t, std::string>
run_process(const std::filesystem::path& program, std::span<const std::string> arguments,
            const std::optional<std::string_view> input, const environment_t environment) {
    std::wstring command = command_line(program, arguments);
    std::wstring block = environment.empty() ? std::wstring() : environment_block(environment);
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

    // The input goes through a pipe of its own, whose read end is the child's.
    HANDLE input_read = nullptr;
    HANDLE input_write = nullptr;
    if (input && CreatePipe(&input_read, &input_write, &security, 0) == 0) {
        CloseHandle(read_end);
        CloseHandle(write_end);
        return std::unexpected(last_error());
    }

    // The child only gets the write end, and the input's read end.
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    if (input) {
        SetHandleInformation(input_write, HANDLE_FLAG_INHERIT, 0);
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input ? input_read : GetStdHandle(STD_INPUT_HANDLE);
    // Stdout and stderr share the pipe, so the output keeps the order it was written in.
    startup.hStdOutput = write_end;
    startup.hStdError = write_end;
    PROCESS_INFORMATION process{};
    const DWORD flags = CREATE_NO_WINDOW | (environment.empty() ? 0 : CREATE_UNICODE_ENVIRONMENT);
    const BOOL started =
        CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, flags,
                       environment.empty() ? nullptr : static_cast<void*>(block.data()), nullptr,
                       &startup, &process);
    CloseHandle(write_end); // the child has its own copy now
    // The input goes in from a thread of its own while the output is read here, so neither
    // side waits on a full pipe for the other. Closing it ends the child's input.
    std::thread writer;
    if (input) {
        CloseHandle(input_read);
        if (started != 0) {
            writer = std::thread([input_write, text = *input] {
                DWORD written = 0;
                WriteFile(input_write, text.data(), static_cast<DWORD>(text.size()), &written,
                          nullptr);
                CloseHandle(input_write);
            });
        } else {
            CloseHandle(input_write);
        }
    }

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

    if (writer.joinable()) {
        writer.join();
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

std::filesystem::path current_executable() {
    std::wstring path(MAX_PATH, L'\0');
    while (true) {
        const DWORD length =
            GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) {
            return {};
        }

        // A path that fills the buffer may have been cut short.
        if (length < path.size()) {
            path.resize(length);
            return path;
        }

        path.resize(path.size() * 2);
    }
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

    // What this process printed so far comes before the child's output. Ctrl+C ends the
    // program, and this process waits for that and gives its exit code, as a shell does.
    std::fflush(nullptr);
    SetConsoleCtrlHandler(ignore_interrupt, TRUE);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                       &startup, &process) == 0) {
        SetConsoleCtrlHandler(ignore_interrupt, FALSE);
        return std::unexpected(std::format("couldn't run {}: {}", program.string(), last_error()));
    }

    WaitForSingleObject(process.hProcess, INFINITE);
    SetConsoleCtrlHandler(ignore_interrupt, FALSE);
    DWORD exit_code = 0;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    return static_cast<int>(exit_code);
}

std::expected<void, std::string> start_detached(const std::filesystem::path& program,
                                                std::span<const std::string> arguments) {
    std::wstring command = command_line(program, arguments);
    // Without STARTF_USESTDHANDLES or inherited handles, it has none of this process's stdin,
    // stdout, or stderr, and no console.
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                       DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &startup,
                       &process) == 0) {
        return std::unexpected(std::format("couldn't run {}: {}", program.string(), last_error()));
    }

    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    return {};
}

#else

namespace {

/// Held while a pipe is made and set to close on exec, and while a program starts: a program
/// another thread starts in between would keep the pipe's ends, as two programs started at once
/// would each other's, and wait on each other's input for good.
std::mutex spawning;

/// A pipe whose ends a program this process starts doesn't keep, but for what it dups them to.
/// It's made under `spawning`.
bool open_pipe(std::array<int, 2>& ends) {
    if (pipe(ends.data()) != 0) {
        return false;
    }

    for (const int end : ends) {
        fcntl(end, F_SETFD, FD_CLOEXEC);
    }

    return true;
}

/// Starts a program, which isn't looked up on PATH; `actions` can be null. It starts with SIGINT,
/// SIGQUIT, and SIGPIPE at their defaults, as one a shell starts has them, though this process
/// ignores the first two while it waits for a program in its terminal, and SIGPIPE once it has
/// written a program's input.
std::expected<pid_t, std::string> spawn(const std::filesystem::path& program,
                                        const std::span<const std::string> arguments,
                                        const posix_spawn_file_actions_t* actions,
                                        const environment_t environment = {}) {
    // posix_spawn takes non-const strings, so it gets copies.
    std::string path = program.string();
    std::vector<std::string> copies(arguments.begin(), arguments.end());
    std::vector<char*> argv{ path.data() };
    for (std::string& argument : copies) {
        argv.push_back(argument.data());
    }

    argv.push_back(nullptr);
    // This process's environment, with `environment`'s variables in place of those of their
    // names.
    std::vector<std::string> entries;
    for (const auto& [name, value] : environment) {
        entries.push_back(name + "=" + value);
    }

    for (char** at = environ; !environment.empty() && *at != nullptr; ++at) {
        const std::string_view entry(*at);
        const std::string_view name = entry.substr(0, entry.find('='));
        if (std::ranges::none_of(environment,
                                 [&](const auto& variable) { return variable.first == name; })) {
            entries.emplace_back(entry);
        }
    }

    std::vector<char*> envp;
    for (std::string& entry : entries) {
        envp.push_back(entry.data());
    }

    envp.push_back(nullptr);
    sigset_t defaults;
    sigemptyset(&defaults);
    for (const int signal_number : { SIGINT, SIGQUIT, SIGPIPE }) {
        sigaddset(&defaults, signal_number);
    }

    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setsigdefault(&attributes, &defaults);
    posix_spawnattr_setflags(&attributes, static_cast<short>(POSIX_SPAWN_SETSIGDEF));
    pid_t pid = 0;
    const int error = posix_spawn(&pid, path.c_str(), actions, &attributes, argv.data(),
                                  environment.empty() ? environ : envp.data());
    posix_spawnattr_destroy(&attributes);
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

std::expected<process_result_t, std::string>
run_process(const std::filesystem::path& program, std::span<const std::string> arguments,
            const std::optional<std::string_view> input, const environment_t environment) {
    std::unique_lock lock(spawning);
    std::array<int, 2> pipe_ends{};
    if (!open_pipe(pipe_ends)) {
        return std::unexpected(std::strerror(errno));
    }

    // The input goes through a pipe of its own, whose read end is the child's stdin.
    std::array<int, 2> input_ends{ -1, -1 };
    if (input && !open_pipe(input_ends)) {
        close(pipe_ends[0]);
        close(pipe_ends[1]);
        return std::unexpected(std::strerror(errno));
    }

    // Stdout and stderr share the pipe, so the output keeps the order it was written in. The
    // child closes its original pipe descriptors and keeps just those two, and its stdin.
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipe_ends[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipe_ends[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipe_ends[0]);
    posix_spawn_file_actions_addclose(&actions, pipe_ends[1]);
    if (input) {
        posix_spawn_file_actions_adddup2(&actions, input_ends[0], STDIN_FILENO);
        posix_spawn_file_actions_addclose(&actions, input_ends[0]);
        posix_spawn_file_actions_addclose(&actions, input_ends[1]);
    }

    const std::expected<pid_t, std::string> pid = spawn(program, arguments, &actions, environment);
    posix_spawn_file_actions_destroy(&actions);
    lock.unlock();
    // Reading only ends once every copy of the write end is closed, and the child has its own.
    close(pipe_ends[1]);
    // The input goes in from a thread of its own while the output is read here, so neither
    // side waits on a full pipe for the other. A child that ends before reading all of it
    // makes the write fail, which mustn't end this process, as SIGPIPE would. Closing it ends
    // the child's input.
    std::thread writer;
    if (input) {
        close(input_ends[0]);
        if (pid) {
            std::signal(SIGPIPE, SIG_IGN);
            writer = std::thread([end = input_ends[1], text = *input] {
                std::size_t done = 0;
                while (done < text.size()) {
                    const ssize_t written = write(end, text.data() + done, text.size() - done);
                    if (written <= 0 && errno != EINTR) {
                        break;
                    }

                    done += written > 0 ? static_cast<std::size_t>(written) : 0;
                }

                close(end);
            });
        } else {
            close(input_ends[1]);
        }
    }

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

    if (writer.joinable()) {
        writer.join();
    }

    close(pipe_ends[0]);
    result.exit_code = wait_for(*pid);
    return result;
}

std::filesystem::path current_executable() {
    std::error_code error;
#ifdef __APPLE__
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string path(size, '\0');
    if (_NSGetExecutablePath(path.data(), &size) != 0) {
        return {};
    }

    path.resize(path.find('\0'));
    return std::filesystem::weakly_canonical(path, error);
#else
    return std::filesystem::read_symlink("/proc/self/exe", error);
#endif
}

std::expected<int, std::string> run_attached(const std::filesystem::path& program,
                                             std::span<const std::string> arguments) {
    // What this process printed so far comes before the child's output. Ctrl+C ends the
    // program, and this process waits for that and gives its exit code, as a shell does.
    std::fflush(nullptr);
    struct sigaction ignore{};
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    struct sigaction interrupt{};
    struct sigaction quit{};
    sigaction(SIGINT, &ignore, &interrupt);
    sigaction(SIGQUIT, &ignore, &quit);
    std::unique_lock lock(spawning);
    const std::expected<pid_t, std::string> pid = spawn(program, arguments, nullptr);
    lock.unlock();
    std::expected<int, std::string> result = pid.transform(wait_for);
    sigaction(SIGINT, &interrupt, nullptr);
    sigaction(SIGQUIT, &quit, nullptr);
    return result;
}

std::expected<void, std::string> start_detached(const std::filesystem::path& program,
                                                std::span<const std::string> arguments) {
    // Its stdin, stdout, and stderr are /dev/null, so nothing it starts, like a browser, keeps a
    // pipe of this process's open.
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    std::unique_lock lock(spawning);
    const std::expected<pid_t, std::string> pid = spawn(program, arguments, &actions);
    lock.unlock();
    posix_spawn_file_actions_destroy(&actions);
    if (!pid) {
        return std::unexpected(pid.error());
    }

    // It's waited for on a thread of its own, so it doesn't stay a zombie, and nothing here
    // waits for it.
    std::thread([pid = *pid] { (void)wait_for(pid); }).detach();
    return {};
}

#endif

std::optional<std::filesystem::path> real_path(const std::filesystem::path& path) {
#ifdef _WIN32
    // libstdc++'s canonical() doesn't follow a junction, but the handle of what it leads to has
    // the final path.
    HANDLE handle =
        CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }

    std::wstring buffer(MAX_PATH, L'\0');
    DWORD length = GetFinalPathNameByHandleW(
        handle, buffer.data(), static_cast<DWORD>(buffer.size()), FILE_NAME_NORMALIZED);
    if (length >= buffer.size()) {
        buffer.resize(length + 1);
        length = GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()),
                                           FILE_NAME_NORMALIZED);
    }

    CloseHandle(handle);
    if (length == 0 || length >= buffer.size()) {
        return std::nullopt;
    }

    buffer.resize(length);
    // It starts "\\?\C:\" or "\\?\UNC\server\share\".
    constexpr std::wstring_view unc = L"\\\\?\\UNC\\";
    constexpr std::wstring_view local = L"\\\\?\\";
    if (buffer.starts_with(unc)) {
        buffer = L"\\\\" + buffer.substr(unc.size());
    } else if (buffer.starts_with(local)) {
        buffer.erase(0, local.size());
    }

    return std::filesystem::path(buffer);
#else
    std::error_code error;
    std::filesystem::path real = std::filesystem::canonical(path, error);
    if (error) {
        return std::nullopt;
    }

    return real;
#endif
}

} // namespace fb
