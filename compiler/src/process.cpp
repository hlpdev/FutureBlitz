#include "fb/process.hpp"

#include "fb/utf8.hpp"

#include <algorithm>
#include <array>
#include <bit>
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
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <mutex>
#include <signal.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/prctl.h>
#endif

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

/// A job whose programs end when its last handle closes, as this process's do when it ends.
HANDLE tying_job() {
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (job != nullptr && SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                                                  sizeof(limits)) == 0) {
        CloseHandle(job);
        return nullptr;
    }

    return job;
}

/// A FILETIME's 100-nanosecond intervals since 1601, as nanoseconds since 1970.
std::int64_t nanoseconds_of(const FILETIME time) {
    constexpr std::int64_t unix_epoch = 116'444'736'000'000'000;
    const std::uint64_t ticks =
        (static_cast<std::uint64_t>(time.dwHighDateTime) << 32U) | time.dwLowDateTime;
    return (static_cast<std::int64_t>(ticks) - unix_epoch) * 100;
}

} // namespace

std::expected<process_result_t, std::string>
run_process(const std::filesystem::path& program, std::span<const std::string> arguments,
            const std::optional<std::string_view> input, const environment_t environment,
            const process_life_t life) {
    std::wstring command = command_line(program, arguments);
    std::wstring block = environment.empty() ? std::wstring() : environment_block(environment);
    // A tied program starts suspended, so it's in the job before it can start another.
    HANDLE job = life == process_life_t::TIED ? tying_job() : nullptr;
    if (life == process_life_t::TIED && job == nullptr) {
        return std::unexpected(std::format("couldn't run {}: {}", program.string(), last_error()));
    }

    const auto close_job = [job] {
        if (job != nullptr) {
            CloseHandle(job);
        }
    };
    SECURITY_ATTRIBUTES security{
        .nLength = sizeof(SECURITY_ATTRIBUTES),
        .lpSecurityDescriptor = nullptr,
        .bInheritHandle = TRUE,
    };
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (CreatePipe(&read_end, &write_end, &security, 0) == 0) {
        const std::string why = last_error();
        close_job();
        return std::unexpected(why);
    }

    // The input goes through a pipe of its own, whose read end is the child's.
    HANDLE input_read = nullptr;
    HANDLE input_write = nullptr;
    if (input && CreatePipe(&input_read, &input_write, &security, 0) == 0) {
        const std::string why = last_error();
        CloseHandle(read_end);
        CloseHandle(write_end);
        close_job();
        return std::unexpected(why);
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
    const DWORD flags = CREATE_NO_WINDOW | (environment.empty() ? 0 : CREATE_UNICODE_ENVIRONMENT) |
                        (job != nullptr ? CREATE_SUSPENDED : 0);
    BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, flags,
                                  environment.empty() ? nullptr : static_cast<void*>(block.data()),
                                  nullptr, &startup, &process);
    std::string why = started == 0 ? last_error() : std::string();
    if (started != 0 && job != nullptr &&
        (AssignProcessToJobObject(job, process.hProcess) == 0 ||
         ResumeThread(process.hThread) == static_cast<DWORD>(-1))) {
        why = last_error();
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hProcess);
        CloseHandle(process.hThread);
        started = 0;
    }

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
        close_job();
        return std::unexpected(std::format("couldn't run {}: {}", program.string(), why));
    }

    process_result_t result;
    // Read everything while it runs: a child with lots of output would block on a full pipe.
    const auto drain = [&result, read_end] {
        std::array<char, 4096> buffer{};
        DWORD count = 0;
        while (ReadFile(read_end, buffer.data(), static_cast<DWORD>(buffer.size()), &count,
                        nullptr) != 0 &&
               count > 0) {
            result.output.append(buffer.data(), count);
        }
    };
    // A tied program's output is read beside it, since what it started and left running can
    // hold the pipe open: once it ends, its job ends them, and then the output ends.
    if (job != nullptr) {
        std::thread reader(drain);
        WaitForSingleObject(process.hProcess, INFINITE);
        TerminateJobObject(job, 1);
        reader.join();
    } else {
        drain();
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
    // What it started and left running ends with the job.
    close_job();
    return result;
}

std::optional<std::int64_t> modified_time(const std::filesystem::path& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) == 0) {
        return std::nullopt;
    }

    return nanoseconds_of(data.ftLastWriteTime);
}

std::int64_t time_now() {
    FILETIME now{};
    GetSystemTimePreciseAsFileTime(&now);
    return nanoseconds_of(now);
}

std::expected<file_lock_t, std::string> file_lock_t::take(const std::filesystem::path& path,
                                                          const std::function<void()>& waiting) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return std::unexpected(std::format("couldn't open {}: {}", to_utf8(path), last_error()));
    }

    // The lock is of its first byte, which needn't be there.
    OVERLAPPED at{};
    if (LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &at) == 0) {
        if (GetLastError() == ERROR_LOCK_VIOLATION && waiting) {
            waiting();
        }

        at = {};
        if (LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &at) == 0) {
            const std::string why = last_error();
            CloseHandle(file);
            return std::unexpected(std::format("couldn't lock {}: {}", to_utf8(path), why));
        }
    }

    return file_lock_t(std::bit_cast<std::intptr_t>(file));
}

file_lock_t::~file_lock_t() {
    if (m_handle != -1) {
        CloseHandle(std::bit_cast<HANDLE>(m_handle));
    }
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

/// The process groups of the tied programs running, each in a slot of its own, 0 for a free one,
/// which a signal handler reads. A program started when they're all taken isn't tied.
std::array<std::atomic<pid_t>, 64> tied_groups{};
/// The signals that end a tied program with this process: a terminal's Ctrl+C and Ctrl+\, its
/// closing, and kill's, and what each did before end_tied() took it.
constexpr std::array tying_signals{ SIGINT, SIGTERM, SIGHUP, SIGQUIT };
std::array<struct sigaction, tying_signals.size()> before_tied{};

/// What `number` did before end_tied() took it.
struct sigaction& before_of(const int number) {
    const auto* at = std::ranges::find(tying_signals, number);
    return before_tied.at(static_cast<std::size_t>(at - tying_signals.begin()));
}

/// The tying signals' handler once a program is tied: it ends the tied programs' groups, then
/// does what the signal did before, which by default ends this process.
void end_tied(const int number, siginfo_t* const info, void* const context) {
    for (const std::atomic<pid_t>& group : tied_groups) {
        if (const pid_t id = group.load(); id > 0) {
            kill(-id, SIGKILL);
        }
    }

    const struct sigaction& before = before_of(number);
    if ((static_cast<unsigned>(before.sa_flags) & static_cast<unsigned>(SA_SIGINFO)) != 0) {
        before.sa_sigaction(number, info, context);
    } else if (before.sa_handler != SIG_DFL) {
        before.sa_handler(number);
    } else {
        // It's blocked while this runs, so it ends the process right after.
        sigaction(number, &before, nullptr);
        raise(number);
    }
}

/// Ends `group` with this process, from the tying signals, but for one this process ignores.
void tie(const pid_t group) {
    static std::once_flag handled;
    std::call_once(handled, [] {
        for (const int number : tying_signals) {
            struct sigaction& before = before_of(number);
            sigaction(number, nullptr, &before);
            const bool information =
                (static_cast<unsigned>(before.sa_flags) & static_cast<unsigned>(SA_SIGINFO)) != 0;
            if (information || before.sa_handler != SIG_IGN) {
                struct sigaction handler{};
                handler.sa_sigaction = end_tied;
                handler.sa_flags = SA_SIGINFO | SA_RESTART;
                sigemptyset(&handler.sa_mask);
                sigaction(number, &handler, nullptr);
            }
        }
    });

    for (std::atomic<pid_t>& slot : tied_groups) {
        pid_t free = 0;
        if (slot.compare_exchange_strong(free, group)) {
            return;
        }
    }
}

void untie(const pid_t group) {
    for (std::atomic<pid_t>& slot : tied_groups) {
        pid_t held = group;
        slot.compare_exchange_strong(held, 0);
    }
}

/// Starts a program, which isn't looked up on PATH; `actions` can be null. It starts with SIGINT,
/// SIGQUIT, and SIGPIPE at their defaults, as one a shell starts has them, though this process
/// ignores the first two while it waits for a program in its terminal, and SIGPIPE once it has
/// written a program's input. A tied one is in a session of its own, its process group's
/// leader, with no terminal, so one that asks on /dev/tty fails at once.
std::expected<pid_t, std::string> spawn(const std::filesystem::path& program,
                                        const std::span<const std::string> arguments,
                                        const posix_spawn_file_actions_t* actions,
                                        const environment_t environment = {},
                                        const process_life_t life = process_life_t::OWN) {
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
    unsigned flags = POSIX_SPAWN_SETSIGDEF;
    if (life == process_life_t::TIED) {
#ifdef POSIX_SPAWN_SETSID
        flags |= static_cast<unsigned>(POSIX_SPAWN_SETSID);
#else
        posix_spawnattr_setpgroup(&attributes, 0);
        flags |= static_cast<unsigned>(POSIX_SPAWN_SETPGROUP);
#endif
    }

    posix_spawnattr_setflags(&attributes, static_cast<short>(flags));
    pid_t pid = 0;
    const int error = posix_spawn(&pid, path.c_str(), actions, &attributes, argv.data(),
                                  environment.empty() ? environ : envp.data());
    posix_spawnattr_destroy(&attributes);
    if (error != 0) {
        return std::unexpected(std::format("couldn't run {}: {}", path, std::strerror(error)));
    }

    return pid;
}

#ifdef __linux__

/// Starts a tied program on Linux with what spawn() gives it, its output on `output`'s write end
/// and its input on `input`'s read end, or this process's for -1, but through a fork, which
/// posix_spawn can't do: with PR_SET_PDEATHSIG, so it ends when this process does, by SIGKILL
/// too. Between the fork and the exec, the child does only what's safe in a process with
/// threads.
std::expected<pid_t, std::string> spawn_tied(const std::filesystem::path& program,
                                             const std::span<const std::string> arguments,
                                             const environment_t environment,
                                             const std::array<int, 2>& output,
                                             const std::array<int, 2>& input) {
    std::string path = program.string();
    std::vector<std::string> copies(arguments.begin(), arguments.end());
    std::vector<char*> argv{ path.data() };
    for (std::string& argument : copies) {
        argv.push_back(argument.data());
    }

    argv.push_back(nullptr);
    std::vector<std::string> entries;
    for (const auto& [name, value] : environment) {
        entries.push_back(name + "=" + value);
    }

    for (char** at = environ; *at != nullptr; ++at) {
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
    // The child says why its exec failed through a pipe that the exec closes.
    std::array<int, 2> report{};
    if (!open_pipe(report)) {
        return std::unexpected(std::format("couldn't run {}: {}", path, std::strerror(errno)));
    }

    const pid_t parent = getpid();
    const pid_t pid = fork();
    if (pid == 0) {
        setsid();
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (getppid() != parent) {
            _exit(127);
        }

        dup2(output[1], STDOUT_FILENO);
        dup2(output[1], STDERR_FILENO);
        if (input[0] >= 0) {
            dup2(input[0], STDIN_FILENO);
        }

        struct sigaction defaults{};
        defaults.sa_handler = SIG_DFL;
        sigemptyset(&defaults.sa_mask);
        for (const int number : { SIGINT, SIGQUIT, SIGPIPE }) {
            sigaction(number, &defaults, nullptr);
        }

        execve(path.c_str(), argv.data(), envp.data());
        const int error = errno;
        if (write(report[1], &error, sizeof(error)) != sizeof(error)) {
            _exit(126);
        }

        _exit(127);
    }

    const int forked = errno;
    close(report[1]);
    if (pid < 0) {
        close(report[0]);
        return std::unexpected(std::format("couldn't run {}: {}", path, std::strerror(forked)));
    }

    int error = 0;
    ssize_t got = 0;
    while ((got = read(report[0], &error, sizeof(error))) < 0 && errno == EINTR) {}

    close(report[0]);
    if (got == sizeof(error)) {
        waitpid(pid, nullptr, 0);
        return std::unexpected(std::format("couldn't run {}: {}", path, std::strerror(error)));
    }

    return pid;
}

#endif

/// The exit code, or 128 plus the signal number when a signal ended it, the way shells report.
int wait_for(const pid_t pid) {
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

} // namespace

std::expected<process_result_t, std::string>
run_process(const std::filesystem::path& program, std::span<const std::string> arguments,
            const std::optional<std::string_view> input, const environment_t environment,
            const process_life_t life) {
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

#ifdef __linux__
    const std::expected<pid_t, std::string> pid =
        life == process_life_t::TIED
            ? spawn_tied(program, arguments, environment, pipe_ends, input_ends)
            : spawn(program, arguments, &actions, environment, life);
#else
    const std::expected<pid_t, std::string> pid =
        spawn(program, arguments, &actions, environment, life);
#endif
    if (pid && life == process_life_t::TIED) {
        tie(*pid);
    }

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
    // Read everything while it runs: a child with lots of output would block on a full pipe.
    const auto drain = [&result, end = pipe_ends[0]] {
        std::array<char, 4096> buffer{};
        ssize_t count = 0;
        while ((count = read(end, buffer.data(), buffer.size())) > 0) {
            result.output.append(buffer.data(), static_cast<std::size_t>(count));
        }
    };
    // A tied program's output is read beside it, since what it started and left running can
    // hold the pipe open: once it ends, so does its group, and then the output ends.
    if (life == process_life_t::TIED) {
        std::thread reader(drain);
        result.exit_code = wait_for(*pid);
        kill(-*pid, SIGKILL);
        untie(*pid);
        reader.join();
    } else {
        drain();
    }

    if (writer.joinable()) {
        writer.join();
    }

    close(pipe_ends[0]);
    if (life != process_life_t::TIED) {
        result.exit_code = wait_for(*pid);
    }

    return result;
}

std::optional<std::int64_t> modified_time(const std::filesystem::path& path) {
    struct stat information{};
    if (stat(path.c_str(), &information) != 0) {
        return std::nullopt;
    }

#ifdef __APPLE__
    const timespec& at = information.st_mtimespec;
#else
    const timespec& at = information.st_mtim;
#endif
    return (static_cast<std::int64_t>(at.tv_sec) * 1'000'000'000) + at.tv_nsec;
}

std::int64_t time_now() {
    timespec now{};
    clock_gettime(CLOCK_REALTIME, &now);
    return (static_cast<std::int64_t>(now.tv_sec) * 1'000'000'000) + now.tv_nsec;
}

std::expected<file_lock_t, std::string> file_lock_t::take(const std::filesystem::path& path,
                                                          const std::function<void()>& waiting) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open's mode is its third argument
    const int file = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (file < 0) {
        return std::unexpected(
            std::format("couldn't open {}: {}", to_utf8(path), std::strerror(errno)));
    }

    if (flock(file, LOCK_EX | LOCK_NB) != 0) {
        if (errno == EWOULDBLOCK && waiting) {
            waiting();
        }

        while (flock(file, LOCK_EX) != 0) {
            if (errno != EINTR) {
                const std::string why = std::strerror(errno);
                close(file);
                return std::unexpected(std::format("couldn't lock {}: {}", to_utf8(path), why));
            }
        }
    }

    return file_lock_t(file);
}

file_lock_t::~file_lock_t() {
    if (m_handle != -1) {
        close(static_cast<int>(m_handle));
    }
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

file_lock_t::file_lock_t(file_lock_t&& other) noexcept
    : m_handle(std::exchange(other.m_handle, -1)) {}

file_lock_t& file_lock_t::operator=(file_lock_t&& other) noexcept {
    if (this != &other) {
        const file_lock_t gone(std::move(*this));
        m_handle = std::exchange(other.m_handle, -1);
    }

    return *this;
}

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
