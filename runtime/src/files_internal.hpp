#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <uv.h>

// What std.io's files share with the runtime's other parts: a file written atomically, which
// std.net.http's downloads to a file write as their bodies come.
namespace fb::runtime {

/// The error a recursive FileWatcher gives on Linux for a tree with more folders than inotify
/// watches, whose message names the setting that says how many.
constexpr std::int64_t too_many_watches = -5001;

/// The error a FileWatcher that isn't recursive gives on Linux when inotify watches as many as
/// it's let, whose message names the setting that says how many.
constexpr std::int64_t no_watches_left = -5002;

/// A file written atomically: a new temporary file beside the one a path leads to, which
/// commit() puts onto the disk and renames over that file, so it holds either all that was
/// written or what it held before. The temporary file goes with this unless it was committed.
class atomic_file_t {
public:
    atomic_file_t() = default;
    atomic_file_t(const atomic_file_t&) = delete;
    atomic_file_t& operator=(const atomic_file_t&) = delete;
    atomic_file_t(atomic_file_t&&) = delete;
    atomic_file_t& operator=(atomic_file_t&&) = delete;
    ~atomic_file_t();

    /// Opens a temporary file for the file at `path`, which is the one a link there leads to,
    /// so the link stays: 0, or libuv's error.
    [[nodiscard]] std::int64_t open(const std::string& path);

    /// Adds `bytes` to the temporary file: 0, or libuv's error.
    [[nodiscard]] std::int64_t write(std::string_view bytes) const;

    /// Puts the temporary file onto the disk and renames it over the file, whose permissions it
    /// takes on POSIX: 0, or libuv's error, when the file is as it was. The temporary file is
    /// gone either way.
    [[nodiscard]] std::int64_t commit();

    /// Removes the temporary file, leaving the file as it was.
    void discard();

private:
    std::string m_path;
    std::string m_temporary;
    uv_file m_file = -1;
};

} // namespace fb::runtime
