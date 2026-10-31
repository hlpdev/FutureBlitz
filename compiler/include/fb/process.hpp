#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace fb {

/// What a finished program returned and printed.
struct process_result_t {
    /// On POSIX, a program killed by a signal gives 128 plus the signal number.
    int exit_code = 0;
    /// What it wrote to stdout and stderr, together.
    std::string output;
};

/// Variables a program's environment has besides this process's, each by its name and value, in
/// place of one of this process's of that name.
using environment_t = std::span<const std::pair<std::string, std::string>>;

/// How long a program run_process() starts can live.
enum class process_life_t : std::uint8_t {
    /// As long as it runs, after this process too.
    OWN,
    /// No longer than this process, with what it starts: on Windows it's in a job that ends
    /// when this process does, however it ends, and on POSIX in a process group of its own,
    /// which SIGINT and SIGTERM to this process end first.
    TIED,
};

/// Runs a program and collects what it prints to stdout and stderr. `arguments` come after the
/// program's own name. With `input`, the program's stdin is that text, of any length, which ends
/// there; without, it's this process's. Its environment is this process's, with `environment`'s
/// variables. Fails only when the program can't be started.
[[nodiscard]] std::expected<process_result_t, std::string>
run_process(const std::filesystem::path& program, std::span<const std::string> arguments,
            std::optional<std::string_view> input = std::nullopt, environment_t environment = {},
            process_life_t life = process_life_t::OWN);

/// A file's last write time, in nanoseconds, to the disk's own precision, which MinGW's
/// std::filesystem rounds to seconds. nullopt when it isn't there.
[[nodiscard]] std::optional<std::int64_t> modified_time(const std::filesystem::path& path);

/// The time now, as modified_time() counts it.
[[nodiscard]] std::int64_t time_now();

/// A lock of a file that one process at a time holds, which the system lets go of when the
/// process ends, however it ends. The file is made when it isn't there, and stays.
class file_lock_t {
public:
    /// Takes the lock, calling `waiting` first when another process holds it, and waits for it
    /// as long as that takes. Fails only when the file can't be opened or locked.
    [[nodiscard]] static std::expected<file_lock_t, std::string>
    take(const std::filesystem::path& path, const std::function<void()>& waiting);

    file_lock_t(const file_lock_t&) = delete;
    file_lock_t& operator=(const file_lock_t&) = delete;
    file_lock_t(file_lock_t&& other) noexcept;
    file_lock_t& operator=(file_lock_t&& other) noexcept;
    ~file_lock_t();

private:
    explicit file_lock_t(const std::intptr_t handle) : m_handle(handle) {}

    /// The file's HANDLE on Windows, or its descriptor, or -1 once it's moved from.
    std::intptr_t m_handle = -1;
};

/// The program that's running, like fb, with its whole path. Empty when the system won't say.
[[nodiscard]] std::filesystem::path current_executable();

/// Where a file or folder really is, as the system has it: with the links and junctions on the
/// way followed, and each name in the disk's own letter case. nullopt when it isn't there.
[[nodiscard]] std::optional<std::filesystem::path> real_path(const std::filesystem::path& path);

/// Runs a program in this terminal, with this process's stdin, stdout, and stderr, and returns
/// its exit code.
[[nodiscard]] std::expected<int, std::string> run_attached(const std::filesystem::path& program,
                                                           std::span<const std::string> arguments);

/// Starts a program and doesn't wait for it, with none of this process's stdin, stdout, or
/// stderr, as a browser is opened. Fails only when it can't be started.
[[nodiscard]] std::expected<void, std::string>
start_detached(const std::filesystem::path& program, std::span<const std::string> arguments);

} // namespace fb
