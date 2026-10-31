// std.io's files, over libuv: whole-file reads and writes, atomic ones too, what's at a path,
// copying, moving, and removing, folders and their listings, temporary files and folders, and
// locks. Each call runs libuv's synchronous file functions on the calling thread, or, for an Async
// twin, on libuv's thread pool, from the I/O thread's loop, which finishes the call's Task.

#include "fb/files.hpp"

#include "fb/runtime.hpp"
#include "files_internal.hpp"
#include "internal.hpp"
#include "io_internal.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <thread>
#include <utility>
#include <uv.h>
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
#include <sys/file.h>
#endif

namespace {

/// What a folder's entry is, as std.io numbers its kinds.
enum kind_t : std::uint8_t {
    KIND_FILE = 0,
    KIND_DIRECTORY = 1,
    KIND_LINK = 2,
    KIND_OTHER = 3,
};

/// An entry of a folder's listing: its path from the folder listed, and its kind.
struct entry_t {
    std::string path;
    std::int32_t kind;
};

/// A time a file keeps: seconds since 1970 began, in UTC, and the nanoseconds after them.
struct file_time_t {
    std::int64_t seconds = 0;
    std::int64_t nanoseconds = 0;
};

/// A lock: the file it's on, open while it's held.
struct lock_t {
    uv_file file;
    bool held = true;
};

/// A file File.Open opened, open until it's closed.
struct stream_t {
    uv_file file;
    bool open = true;
};

/// What File.Open's `how` says: to read, to write, to write at the end, to make the file when it
/// isn't there, to make it and refuse one that's there, and to empty it.
constexpr std::uint32_t open_read = 1;
constexpr std::uint32_t open_write = 2;
constexpr std::uint32_t open_append = 4;
constexpr std::uint32_t open_create = 8;
constexpr std::uint32_t open_create_new = 16;
constexpr std::uint32_t open_truncate = 32;

/// A read that would hold more bytes than its most, which fb_io_error_message tells of.
constexpr std::int64_t too_big = -5000;

/// The bytes a read gives, in memory C's malloc gives, which says when there isn't enough
/// instead of throwing, as nothing in the runtime can, so a file too big for memory is an error
/// and not the program's end.
class buffer_t {
public:
    /// Room for `capacity` bytes in all: false when there isn't that much memory.
    [[nodiscard]] bool reserve(const std::size_t capacity) {
        if (capacity <= m_capacity) {
            return true;
        }

        auto* grown = static_cast<char*>(std::malloc(capacity));
        if (grown == nullptr) {
            return false;
        }

        if (m_size > 0) {
            std::memcpy(grown, m_data.get(), m_size);
        }

        m_data.reset(grown);
        m_capacity = capacity;
        return true;
    }

    void clear() {
        m_data.reset();
        m_size = 0;
        m_capacity = 0;
    }

    /// Where the next bytes go, and how many more fit.
    [[nodiscard]] char* end() {
        return m_data.get() + m_size;
    }

    [[nodiscard]] std::size_t room() const {
        return m_capacity - m_size;
    }

    void add(const std::size_t count) {
        m_size += count;
    }

    [[nodiscard]] std::size_t size() const {
        return m_size;
    }

    [[nodiscard]] std::string_view view() const {
        return { m_data.get(), m_size };
    }

private:
    struct free_t {
        void operator()(char* data) const {
            std::free(data);
        }
    };

    std::unique_ptr<char, free_t> m_data;
    std::size_t m_size = 0;
    std::size_t m_capacity = 0;
};

struct request_t;

/// What a call does, given its request, on whichever thread it runs.
using work_t = std::int64_t (*)(request_t& request);

/// One call's arguments and what it gives, which the caller and, for an Async twin, the I/O
/// thread both hold until they're done with it.
struct request_t {
    std::atomic<std::int32_t> owners = 1;
    /// What the call is given: a path, a second one for a copy or a move, the bytes a write
    /// writes, and a number, like a write's how or a listing's depth.
    std::string path;
    std::string other;
    /// What a write writes: the caller's own bytes for a call that's done before it returns, or
    /// `owned`, a copy, for an Async twin's.
    std::string_view input;
    std::string owned;
    std::int64_t number = 0;
    std::int32_t flag = 0;
    /// What it gives.
    buffer_t bytes;
    std::vector<entry_t> entries;
    uv_stat_t stat{};
    /// What a stat found the file changed, read, and made at.
    std::array<file_time_t, 3> times{};
    lock_t* lock = nullptr;
    /// The file an open made, until fb_io_request_stream takes it, and the one a stream's call is
    /// on, with where in it the call reads or writes, or -1 for where it is.
    stream_t* stream = nullptr;
    uv_file file = -1;
    std::int64_t offset = -1;
    /// An Async twin's work, Task, and libuv request.
    work_t work = nullptr;
    std::int64_t result = 0;
    fb_task_t* task = nullptr;
    uv_work_t queued{};
};

void release(request_t* request) {
    if (request->owners.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        if (request->lock != nullptr) {
            fb_io_lock_free(request->lock);
        }

        if (request->stream != nullptr) {
            fb_io_stream_free(request->stream);
        }

        delete request;
    }
}

std::string_view view_of(const fb_string_t* text) {
    return { reinterpret_cast<const char*>(text + 1), static_cast<std::size_t>(text->length) };
}

/// A path as the request keeps it, given or made, which libuv reads up to its NUL: so one that
/// holds a NUL would be another path, and is refused.
bool has_nul(const std::string& path) {
    return path.contains('\0');
}

/// libuv's request for one synchronous file call, cleaned up after it.
class fs_t {
public:
    fs_t() = default;
    fs_t(const fs_t&) = delete;
    fs_t& operator=(const fs_t&) = delete;
    fs_t(fs_t&&) = delete;
    fs_t& operator=(fs_t&&) = delete;

    ~fs_t() {
        uv_fs_req_cleanup(&m_request);
    }

    uv_fs_t* get() {
        return &m_request;
    }

private:
    uv_fs_t m_request{};
};

/// `call`'s `result`, or a Windows error libuv doesn't know as the one it is: replacing or
/// shortening a mapped file is busy.
std::int64_t known_error(const uv_fs_t* call, const std::int64_t result) {
#ifdef _WIN32
    if (result == UV_UNKNOWN && uv_fs_get_system_error(call) == ERROR_USER_MAPPED_FILE) {
        return UV_EBUSY;
    }
#else
    static_cast<void>(call);
#endif
    return result;
}

std::int64_t open_file(const std::string& path, const int flags, const int mode) {
    fs_t call;
    return known_error(call.get(),
                       uv_fs_open(nullptr, call.get(), path.c_str(), flags, mode, nullptr));
}

void close_file(const uv_file file) {
    fs_t call;
    uv_fs_close(nullptr, call.get(), file, nullptr);
}

/// What's at `path`, into `stat`, through a link unless `follow` is false.
std::int64_t stat_path(const std::string& path, const bool follow, uv_stat_t& stat) {
    fs_t call;
    const int result = follow ? uv_fs_stat(nullptr, call.get(), path.c_str(), nullptr)
                              : uv_fs_lstat(nullptr, call.get(), path.c_str(), nullptr);
    if (result == 0) {
        stat = call.get()->statbuf;
    }

    return result;
}

std::int64_t stat_file(const uv_file file, uv_stat_t& stat) {
    fs_t call;
    const int result = uv_fs_fstat(nullptr, call.get(), file, nullptr);
    if (result == 0) {
        stat = call.get()->statbuf;
    }

    return result;
}

bool is_directory(const uv_stat_t& stat) {
    return (stat.st_mode & S_IFMT) == S_IFDIR;
}

std::int32_t kind_of(const uv_stat_t& stat) {
    switch (stat.st_mode & S_IFMT) {
    case S_IFREG:
        return KIND_FILE;
    case S_IFDIR:
        return KIND_DIRECTORY;
    case S_IFLNK:
        return KIND_LINK;
    default:
        return KIND_OTHER;
    }
}

/// Empties the open file. Opening it with O_TRUNC would, but Windows opens a hidden or system
/// file to be emptied, as libuv asks, only when the call gives it the same attributes, and one
/// that has to be there only for a GENERIC_WRITE that libuv doesn't ask for.
std::int64_t empty_file(const uv_file file) {
    fs_t call;
    return known_error(call.get(), uv_fs_ftruncate(nullptr, call.get(), file, 0, nullptr));
}

/// Writes all of `bytes` to `file`, from where it is.
std::int64_t write_all(const uv_file file, std::string_view bytes) {
    while (!bytes.empty()) {
        // libuv takes a buffer's size as an unsigned int.
        const std::size_t size = std::min<std::size_t>(bytes.size(), 1U << 30U);
        const uv_buf_t buffer =
            uv_buf_init(const_cast<char*>(bytes.data()), static_cast<unsigned int>(size));
        fs_t call;
        const int written = uv_fs_write(nullptr, call.get(), file, &buffer, 1, -1, nullptr);
        if (written < 0) {
            return written;
        }

        bytes.remove_prefix(static_cast<std::size_t>(written));
    }

    return 0;
}

std::int64_t read_file(request_t& request) {
    if (has_nul(request.path)) {
        return UV_EINVAL;
    }

    const std::int64_t opened = open_file(request.path, UV_FS_O_RDONLY, 0);
    if (opened < 0) {
        return opened;
    }

    const auto file = static_cast<uv_file>(opened);
    uv_stat_t stat{};
    std::int64_t result = stat_file(file, stat);
    if (result == 0 && is_directory(stat)) {
        result = UV_EISDIR;
    }

    // The most the read may hold is the request's number; one more byte than that, which a
    // file that says it's smaller can still give, is too many.
    const auto most = static_cast<std::uint64_t>(request.number);
    if (result == 0 && stat.st_size > most) {
        result = too_big;
    }

    // What fstat says is only where to start: a file can grow while it's read, or say 0, as
    // Linux's /proc files do. A read past the end, which gives 0, ends it.
    buffer_t& bytes = request.bytes;
    bytes.clear();
    if (result == 0 && !bytes.reserve(static_cast<std::size_t>(stat.st_size) + 1)) {
        result = UV_ENOMEM;
    }

    while (result == 0) {
        if (bytes.size() > most) {
            result = too_big;
            break;
        }

        if (bytes.room() == 0) {
            const std::size_t grown = bytes.size() + std::max<std::size_t>(bytes.size() / 2, 65536);
            if (!bytes.reserve(std::min<std::uint64_t>(grown, most + 1))) {
                result = UV_ENOMEM;
                break;
            }
        }

        // libuv takes a buffer's size as an unsigned int.
        const std::size_t room = std::min<std::size_t>(bytes.room(), 1U << 30U);
        const uv_buf_t buffer = uv_buf_init(bytes.end(), static_cast<unsigned int>(room));
        fs_t call;
        const int count = uv_fs_read(nullptr, call.get(), file, &buffer, 1, -1, nullptr);
        if (count < 0) {
            result = count;
        } else if (count == 0) {
            break;
        } else {
            bytes.add(static_cast<std::size_t>(count));
        }
    }

    close_file(file);
    if (result < 0) {
        bytes.clear();
        return result;
    }

    return static_cast<std::int64_t>(bytes.size());
}

/// The folder `path` is in, and its last name, by the target's separators.
std::pair<std::string, std::string> split_path(const std::string& path) {
#ifdef _WIN32
    const std::size_t at = path.find_last_of("/\\:");
#else
    const std::size_t at = path.find_last_of('/');
#endif
    if (at == std::string::npos) {
        return { ".", path };
    }

    return { path.substr(0, at + 1), path.substr(at + 1) };
}

/// A name for a temporary file beside `path`'s: ".fb-" and 12 letters and digits mixed from the
/// process, a count, and the clock, which atomic_file_t opens only if no file has it. It's short,
/// so a file whose own name is long can still be written atomically.
std::string temporary_beside(const std::string& path) {
    static std::atomic<std::uint64_t> made = 0;
    std::string folder = split_path(path).first;
    if (folder == ".") {
        folder.clear();
    }

    // SplitMix64's mix, which spreads every bit of each part over the whole.
    std::uint64_t mixed = (static_cast<std::uint64_t>(uv_os_getpid()) << 32U) ^ uv_hrtime() ^
                          (made.fetch_add(1, std::memory_order_relaxed) * 0x9E3779B97F4A7C15U);
    mixed = (mixed ^ (mixed >> 30U)) * 0xBF58476D1CE4E5B9U;
    mixed = (mixed ^ (mixed >> 27U)) * 0x94D049BB133111EBU;
    mixed ^= mixed >> 31U;
    constexpr std::string_view digits = "0123456789abcdefghijklmnopqrstuvwxyz";
    std::string temporary = folder + ".fb-";
    for (int i = 0; i < 12; ++i) {
        temporary += digits[mixed % digits.size()];
        mixed /= digits.size();
    }

    return temporary + ".tmp";
}

#ifdef _WIN32
/// FILE_RENAME_INFO as Windows 10 1709's SDK has it, with Flags in place of ReplaceIfExists,
/// which older MinGW headers lack, and FileRenameInfoEx's flags.
struct rename_info_t {
    DWORD flags;
    HANDLE root;
    DWORD length;
    WCHAR name[1];
};

constexpr DWORD rename_replace_if_exists = 0x1;
constexpr DWORD rename_posix_semantics = 0x2;
constexpr auto file_rename_info_ex = static_cast<FILE_INFO_BY_HANDLE_CLASS>(22);

/// Renames the file at `from` over the one at `to` with POSIX semantics, which replaces a file
/// another handle holds open, sharing delete, as std's do, and which goes on reading the old
/// one. Where Windows or the disk doesn't have them (before Windows 10 1709, or FAT), it's
/// libuv's rename, MoveFileExW's, which can't.
std::int64_t replace_file(const std::string& from, const std::string& to) {
    const auto rename = [&] {
        fs_t call;
        return uv_fs_rename(nullptr, call.get(), from.c_str(), to.c_str(), nullptr);
    };
    const auto wide = [](const std::string& path) {
        const int length = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
        std::wstring text(static_cast<std::size_t>(std::max(length, 1)), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, text.data(), length);
        text.resize(text.size() - 1);
        return text;
    };

    // The new name is a whole path, as the handle's rename takes it.
    const std::wstring target = wide(to);
    const DWORD needed = GetFullPathNameW(target.c_str(), 0, nullptr, nullptr);
    std::wstring full(needed, L'\0');
    const DWORD written = GetFullPathNameW(target.c_str(), needed, full.data(), nullptr);
    if (needed == 0 || written == 0 || written >= needed) {
        return rename();
    }

    full.resize(written);
    HANDLE file = CreateFileW(wide(from).c_str(), DELETE | SYNCHRONIZE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return rename();
    }

    const std::size_t size = offsetof(rename_info_t, name) + ((full.size() + 1) * sizeof(WCHAR));
    std::vector<std::byte> buffer(size);
    auto* info = reinterpret_cast<rename_info_t*>(buffer.data());
    info->flags = rename_replace_if_exists | rename_posix_semantics;
    info->root = nullptr;
    info->length = static_cast<DWORD>(full.size() * sizeof(WCHAR));
    std::memcpy(static_cast<void*>(info->name), full.c_str(), (full.size() + 1) * sizeof(WCHAR));
    const BOOL renamed =
        SetFileInformationByHandle(file, file_rename_info_ex, info, static_cast<DWORD>(size));
    const DWORD error = renamed != 0 ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (renamed != 0) {
        return 0;
    }

    // A Windows or a disk without them says it doesn't know the class or its flags.
    if (error == ERROR_INVALID_PARAMETER || error == ERROR_NOT_SUPPORTED ||
        error == ERROR_INVALID_FUNCTION) {
        return rename();
    }

    return uv_translate_sys_error(static_cast<int>(error));
}
#endif

/// Whether a link's target `target` is from a root, not from the link's folder.
bool is_rooted(const std::string_view target) {
#ifdef _WIN32
    return target.starts_with('/') || target.starts_with('\\') ||
           (target.size() > 1 && target[1] == ':');
#else
    return target.starts_with('/');
#endif
}

/// The file a write to `path` changes: the one its link leads to, through each link after it,
/// which needn't be there yet, or `path` itself when it isn't a link.
std::string linked_file(std::string path) {
    // As many links as Linux follows before it says there's a loop.
    for (int links = 0; links < 40; ++links) {
        uv_stat_t found{};
        if (stat_path(path, false, found) != 0 || kind_of(found) != KIND_LINK) {
            break;
        }

        fs_t call;
        if (uv_fs_readlink(nullptr, call.get(), path.c_str(), nullptr) != 0) {
            break;
        }

        const std::string target = static_cast<const char*>(call.get()->ptr);
        const std::string folder = split_path(path).first;
        path = is_rooted(target) || folder == "." ? target : folder + target;
    }

    return path;
}

/// Writes `bytes` to a new temporary file beside `path`'s, onto the disk, and renames it over
/// that file, as atomic_file_t does.
std::int64_t write_atomic(const std::string& path, const std::string_view bytes) {
    fb::runtime::atomic_file_t file;
    std::int64_t result = file.open(path);
    if (result == 0) {
        result = file.write(bytes);
    }

    return result == 0 ? file.commit() : result;
}

/// Writes the request's input to its path, as its number says: replacing, adding to the end, or
/// atomically.
std::int64_t write_file(const request_t& request) {
    if (has_nul(request.path)) {
        return UV_EINVAL;
    }

    if (request.number == 2) {
        return write_atomic(request.path, request.input);
    }

    // Opening a folder to write is EINVAL on Windows and EISDIR on POSIX.
    if (uv_stat_t found{}; stat_path(request.path, true, found) == 0 && is_directory(found)) {
        return UV_EISDIR;
    }

    const bool appends = request.number == 1;
    const int flags = UV_FS_O_WRONLY | UV_FS_O_CREAT | (appends ? UV_FS_O_APPEND : 0);
    const std::int64_t opened = open_file(request.path, flags, 0666);
    if (opened < 0) {
        return opened;
    }

    const auto file = static_cast<uv_file>(opened);
    std::int64_t result = appends ? 0 : empty_file(file);
    if (result == 0) {
        result = write_all(file, request.input);
    }

    close_file(file);
    return result;
}

#ifdef _WIN32
/// A FILETIME, 100-nanosecond ticks since 1601 began, as a time since 1970.
file_time_t time_of(const FILETIME& time) {
    constexpr std::int64_t ticks_before_1970 = 116'444'736'000'000'000;
    constexpr std::int64_t ticks_a_second = 10'000'000;
    const auto ticks =
        static_cast<std::int64_t>((static_cast<std::uint64_t>(time.dwHighDateTime) << 32U) |
                                  time.dwLowDateTime) -
        ticks_before_1970;
    const std::int64_t seconds = (ticks / ticks_a_second) - (ticks % ticks_a_second < 0 ? 1 : 0);
    return { .seconds = seconds, .nanoseconds = (ticks - (seconds * ticks_a_second)) * 100 };
}

/// The times Windows keeps for `path`, through a link when `follow` is, into `times`; libuv's
/// keep seconds in a 32-bit long there, which runs out in 2038. Leaves them for a file Windows
/// won't open.
void windows_times(const std::string& path, const bool follow, std::array<file_time_t, 3>& times) {
    const int length = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (length <= 0) {
        return;
    }

    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wide.data(), length);
    const DWORD flags = FILE_FLAG_BACKUP_SEMANTICS | (follow ? 0 : FILE_FLAG_OPEN_REPARSE_POINT);
    HANDLE file = CreateFileW(wide.c_str(), FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, flags, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }

    FILETIME made{};
    FILETIME read{};
    FILETIME changed{};
    if (GetFileTime(file, &made, &read, &changed) != 0) {
        times = { time_of(changed), time_of(read), time_of(made) };
    }

    CloseHandle(file);
}
#endif

std::int64_t stat_request(request_t& request) {
    if (has_nul(request.path)) {
        return UV_EINVAL;
    }

    const bool follow = request.flag != 0;
    const std::int64_t found = stat_path(request.path, follow, request.stat);
    if (found < 0) {
        return found;
    }

    const auto time_of_stat = [](const uv_timespec_t& time) {
        return file_time_t{
            .seconds = static_cast<std::int64_t>(time.tv_sec),
            .nanoseconds = static_cast<std::int64_t>(time.tv_nsec),
        };
    };
    request.times = {
        time_of_stat(request.stat.st_mtim),
        time_of_stat(request.stat.st_atim),
        time_of_stat(request.stat.st_birthtim),
    };
#ifdef _WIN32
    windows_times(request.path, follow, request.times);
#endif
    return found;
}

std::int64_t copy_file(const std::string& from, const std::string& to) {
    fs_t call;
    return uv_fs_copyfile(nullptr, call.get(), from.c_str(), to.c_str(), 0, nullptr);
}

std::int64_t copy_request(const request_t& request) {
    if (has_nul(request.path) || has_nul(request.other)) {
        return UV_EINVAL;
    }

    uv_stat_t stat{};
    if (const std::int64_t found = stat_path(request.path, true, stat); found < 0) {
        return found;
    }

    // Windows' copy onto a folder is ACCESS_DENIED, where POSIX's is EISDIR.
    uv_stat_t onto{};
    if (is_directory(stat) || (stat_path(request.other, true, onto) == 0 && is_directory(onto))) {
        return UV_EISDIR;
    }

    return copy_file(request.path, request.other);
}

std::int64_t remove_file(const std::string& path) {
    fs_t call;
    return uv_fs_unlink(nullptr, call.get(), path.c_str(), nullptr);
}

std::int64_t move_request(const request_t& request) {
    if (has_nul(request.path) || has_nul(request.other)) {
        return UV_EINVAL;
    }

    // What a rename does with a folder at either end differs: Windows replaces a file with a
    // folder, and POSIX a folder with an empty one. Here a folder only goes where nothing is, a
    // file never replaces a folder, and a rename onto the same file, as one of its case is, is
    // left to the OS.
    uv_stat_t from{};
    uv_stat_t onto{};
    if (stat_path(request.path, false, from) == 0 && stat_path(request.other, false, onto) == 0 &&
        (from.st_dev != onto.st_dev || from.st_ino != onto.st_ino)) {
        if (is_directory(from)) {
            return is_directory(onto) ? UV_EEXIST : UV_ENOTDIR;
        }

        if (is_directory(onto)) {
            return UV_EISDIR;
        }
    }

    std::int64_t result = 0;
    {
        fs_t call;
        result =
            uv_fs_rename(nullptr, call.get(), request.path.c_str(), request.other.c_str(), nullptr);
    }

    // Another disk can't take a rename, so a file goes as a copy instead.
    uv_stat_t stat{};
    if (result == UV_EXDEV && stat_path(request.path, false, stat) == 0 && !is_directory(stat)) {
        result = copy_file(request.path, request.other);
        if (result == 0) {
            result = remove_file(request.path);
        }
    }

    return result;
}

std::int64_t delete_request(const request_t& request) {
    if (has_nul(request.path)) {
        return UV_EINVAL;
    }

    uv_stat_t stat{};
    if (const std::int64_t found = stat_path(request.path, false, stat); found < 0) {
        return found == UV_ENOENT ? 0 : found;
    }

    if (is_directory(stat)) {
        return UV_EISDIR;
    }

    const std::int64_t removed = remove_file(request.path);
    return removed < 0 ? removed : 1;
}

std::int64_t set_read_only(const request_t& request) {
    if (has_nul(request.path)) {
        return UV_EINVAL;
    }

    uv_stat_t stat{};
    if (const std::int64_t found = stat_path(request.path, true, stat); found < 0) {
        return found;
    }

    // On Windows, libuv sets the read-only attribute when the mode has no write bit for the
    // owner.
    const std::uint64_t mode = request.flag != 0 ? stat.st_mode & ~0222U : stat.st_mode | 0200U;
    fs_t call;
    return uv_fs_chmod(nullptr, call.get(), request.path.c_str(), static_cast<int>(mode & 07777U),
                       nullptr);
}

std::int64_t make_folder(const std::string& path) {
    fs_t call;
    return uv_fs_mkdir(nullptr, call.get(), path.c_str(), 0777, nullptr);
}

/// `path` without the separators at its end, but for a root's.
std::string trimmed(std::string path) {
#ifdef _WIN32
    constexpr std::string_view separators = "/\\";
#else
    constexpr std::string_view separators = "/";
#endif
    while (path.size() > 1 && separators.contains(path.back())) {
#ifdef _WIN32
        if (path.size() == 3 && path[1] == ':') {
            break;
        }
#endif
        path.pop_back();
    }

    return path;
}

/// Makes the folder at `path`, and the ones it's in: 1, or 0 when it was there.
std::int64_t make_folders(const std::string& path) {
    const std::int64_t made = make_folder(path);
    if (made == 0) {
        return 1;
    }

    uv_stat_t stat{};
    if (made == UV_EEXIST || made == UV_EPERM || made == UV_EACCES) {
        // A root, or a folder made meanwhile, is there, and a file isn't a folder.
        if (stat_path(path, true, stat) == 0) {
            return is_directory(stat) ? 0 : UV_ENOTDIR;
        }

        return made;
    }

    if (made != UV_ENOENT) {
        return made;
    }

    const std::string folder = trimmed(split_path(trimmed(path)).first);
    if (folder == "." || folder == path || folder.size() >= trimmed(path).size()) {
        return made;
    }

    if (const std::int64_t above = make_folders(folder); above < 0) {
        return above;
    }

    const std::int64_t again = make_folder(path);
    if (again == UV_EEXIST && stat_path(path, true, stat) == 0 && is_directory(stat)) {
        return 0;
    }

    return again < 0 ? again : 1;
}

std::int64_t create_request(const request_t& request) {
    if (has_nul(request.path)) {
        return UV_EINVAL;
    }

    if (request.flag != 0) {
        return make_folders(trimmed(request.path));
    }

    const std::int64_t made = make_folder(request.path);
    return made < 0 ? made : 1;
}

/// The entries of the folder at `path`, each's name and kind, in the order of their names'
/// bytes, from libuv's or, where it doesn't say, the OS's.
std::int64_t list_folder(const std::string& path, std::vector<entry_t>& entries) {
    fs_t call;
    const int count = uv_fs_scandir(nullptr, call.get(), path.c_str(), 0, nullptr);
    if (count < 0) {
        return count;
    }

    uv_dirent_t entry{};
    while (uv_fs_scandir_next(call.get(), &entry) != UV_EOF) {
        std::int32_t kind = KIND_OTHER;
        switch (entry.type) {
        case UV_DIRENT_FILE:
            kind = KIND_FILE;
            break;
        case UV_DIRENT_DIR:
            kind = KIND_DIRECTORY;
            break;
        case UV_DIRENT_LINK:
            kind = KIND_LINK;
            break;
        case UV_DIRENT_UNKNOWN: {
            uv_stat_t stat{};
            if (stat_path(path + "/" + entry.name, false, stat) == 0) {
                kind = kind_of(stat);
            }

            break;
        }
        default:
            break;
        }

        entries.push_back({ .path = entry.name, .kind = kind });
    }

    std::ranges::sort(entries, {}, &entry_t::path);
    return 0;
}

/// The text of a call that gives a path, with '/' between names on every OS.
std::string with_slashes(std::string path) {
#ifdef _WIN32
    std::ranges::replace(path, '\\', '/');
#endif
    return path;
}

/// Adds the entries of the folder at `folder`, by their paths from the folder first listed
/// (`prefix` is this one's), each before what's in it, to `depth` more folders down, or all of
/// them below 1.
std::int64_t walk(const std::string& folder, const std::string& prefix, const std::int64_t depth,
                  std::vector<entry_t>& into, std::string& failed) {
    std::vector<entry_t> entries;
    if (const std::int64_t listed = list_folder(folder, entries); listed < 0) {
        failed = with_slashes(folder);
        return listed;
    }

    for (entry_t& entry : entries) {
        const std::string name = std::move(entry.path);
        into.push_back({ .path = prefix + name, .kind = entry.kind });
        if (entry.kind == KIND_DIRECTORY && depth != 1) {
            std::string inside = folder;
            inside.append("/").append(name);
            std::string from = prefix;
            from.append(name).append("/");
            const std::int64_t walked =
                walk(inside, from, depth < 1 ? depth : depth - 1, into, failed);
            if (walked < 0) {
                return walked;
            }
        }
    }

    return 0;
}

std::int64_t entries_request(request_t& request) {
    if (has_nul(request.path)) {
        return UV_EINVAL;
    }

    request.entries.clear();
    const std::string folder = trimmed(request.path);
    uv_stat_t stat{};
    if (const std::int64_t found = stat_path(folder, true, stat); found < 0) {
        return found;
    }

    if (!is_directory(stat)) {
        return UV_ENOTDIR;
    }

    request.other.clear();
    const std::int64_t walked = walk(folder, "", request.number, request.entries, request.other);
    return walked < 0 ? walked : static_cast<std::int64_t>(request.entries.size());
}

std::int64_t remove_folder(const std::string& path) {
    fs_t call;
    return uv_fs_rmdir(nullptr, call.get(), path.c_str(), nullptr);
}

/// Removes what's at `path`, and what's in it, without following links.
std::int64_t remove_all(const std::string& path) {
    uv_stat_t stat{};
    if (const std::int64_t found = stat_path(path, false, stat); found < 0) {
        return found;
    }

    if (!is_directory(stat)) {
        return remove_file(path);
    }

    std::vector<entry_t> entries;
    if (const std::int64_t listed = list_folder(path, entries); listed < 0) {
        return listed;
    }

    for (const entry_t& entry : entries) {
        if (const std::int64_t removed = remove_all(path + "/" + entry.path); removed < 0) {
            return removed;
        }
    }

    return remove_folder(path);
}

std::int64_t remove_request(const request_t& request) {
    if (has_nul(request.path)) {
        return UV_EINVAL;
    }

    const std::string path = trimmed(request.path);
    uv_stat_t stat{};
    if (const std::int64_t found = stat_path(path, false, stat); found < 0) {
        return found == UV_ENOENT ? 0 : found;
    }

    if (!is_directory(stat)) {
        return UV_ENOTDIR;
    }

    const std::int64_t removed = request.flag != 0 ? remove_all(path) : remove_folder(path);
    return removed < 0 ? removed : 1;
}

/// What a call of libuv's that fills a buffer with a path gives, or nothing.
template <typename Call>
std::optional<std::string> path_from(Call call) {
    std::array<char, 4096> buffer{};
    std::size_t size = buffer.size();
    int result = call(buffer.data(), &size);
    if (result == UV_ENOBUFS) {
        std::string bigger(size, '\0');
        result = call(bigger.data(), &size);
        if (result == 0) {
            bigger.resize(size);
            return with_slashes(std::move(bigger));
        }
    }

    if (result != 0) {
        return std::nullopt;
    }

    return with_slashes(std::string(buffer.data(), size));
}

fb_string_t* string_of(const std::string_view text) {
    return fb_string_from_bytes(text.data(), static_cast<std::int64_t>(text.size()));
}

#ifdef _WIN32
/// Where a lock is: a byte so far past where files end that locking it never keeps anyone from
/// reading or writing the file, which Windows' locks would.
constexpr DWORD lock_offset_high = 0x7FFFFFFF;
constexpr DWORD lock_offset_low = 0xFFFFFFFE;
#endif

std::int64_t acquire_lock(request_t& request) {
    if (has_nul(request.path)) {
        return UV_EINVAL;
    }

    // Opening a folder to write is EINVAL on Windows and EISDIR on POSIX.
    if (uv_stat_t found{}; stat_path(request.path, true, found) == 0 && is_directory(found)) {
        return UV_EISDIR;
    }

    // A lock needs no writing, so a file that's there but read-only is opened to read.
    std::int64_t opened = open_file(request.path, UV_FS_O_RDWR | UV_FS_O_CREAT, 0666);
    if (opened == UV_EACCES || opened == UV_EPERM) {
        opened = open_file(request.path, UV_FS_O_RDONLY, 0);
    }

    if (opened < 0) {
        return opened;
    }

    const auto file = static_cast<uv_file>(opened);
    const bool shared = request.flag != 0;
    const bool wait = request.number != 0;
#ifdef _WIN32
    OVERLAPPED overlapped{};
    overlapped.Offset = lock_offset_low;
    overlapped.OffsetHigh = lock_offset_high;
    constexpr DWORD exclusive = LOCKFILE_EXCLUSIVE_LOCK;
    constexpr DWORD immediately = LOCKFILE_FAIL_IMMEDIATELY;
    const DWORD flags = (shared ? 0U : exclusive) | (wait ? 0U : immediately);
    if (LockFileEx(uv_get_osfhandle(file), flags, 0, 1, 0, &overlapped) == 0) {
        const DWORD error = GetLastError();
        close_file(file);
        return error == ERROR_LOCK_VIOLATION || error == ERROR_IO_PENDING
                   ? 0
                   : uv_translate_sys_error(static_cast<int>(error));
    }
#else
    int locked = 0;
#ifdef __APPLE__
    // macOS wakes only some of the flocks waiting for a lock that's let go, and the rest wait on
    // until another is, which may be never. So a lock that waits tries again without waiting,
    // a little longer apart each time, as it would be woken.
    std::chrono::milliseconds pause{ 1 };
    while (true) {
        locked = flock(file, (shared ? LOCK_SH : LOCK_EX) | LOCK_NB);
        if (locked == 0 || !wait || (errno != EWOULDBLOCK && errno != EINTR)) {
            break;
        }

        std::this_thread::sleep_for(pause);
        pause = std::min(pause * 2, std::chrono::milliseconds{ 50 });
    }
#else
    do {
        locked = flock(file, (shared ? LOCK_SH : LOCK_EX) | (wait ? 0 : LOCK_NB));
    } while (locked != 0 && errno == EINTR);
#endif

    if (locked != 0) {
        const int error = errno;
        close_file(file);
        return error == EWOULDBLOCK ? 0 : uv_translate_sys_error(error);
    }
#endif

    request.lock = new lock_t{ .file = file };
    return 1;
}

/// One of libuv's open flags, as bits that add up to its flags.
constexpr std::uint32_t flag_of(const int flag) {
    return static_cast<std::uint32_t>(flag);
}

std::int64_t open_request(request_t& request) {
    if (has_nul(request.path)) {
        return UV_EINVAL;
    }

    const auto how = static_cast<std::uint32_t>(request.flag);
    const bool reads = (how & open_read) != 0;
    const bool writes = (how & (open_write | open_append)) != 0;
    const bool appends = (how & open_append) != 0;
    const bool exclusive = (how & open_create_new) != 0;
    const bool creates = exclusive || (how & open_create) != 0;
    // A file is emptied once it's open, as empty_file says why; a new one is empty already.
    const bool truncates = (how & open_truncate) != 0 && !exclusive;
    std::uint32_t flags = flag_of(UV_FS_O_RDONLY);
    if (writes) {
        flags = flag_of(reads ? UV_FS_O_RDWR : UV_FS_O_WRONLY);
    }

    if (appends) {
        flags |= flag_of(UV_FS_O_APPEND);
    }

    if (creates) {
        flags |= flag_of(UV_FS_O_CREAT);
    }

    if (exclusive) {
        flags |= flag_of(UV_FS_O_EXCL);
    }

    // Opening a folder to write is EINVAL on Windows and EISDIR on POSIX, and to read works on
    // POSIX, so a folder is looked for before and after.
    if (uv_stat_t found{}; stat_path(request.path, true, found) == 0 && is_directory(found)) {
        return UV_EISDIR;
    }

    const std::int64_t opened = open_file(request.path, static_cast<int>(flags), 0666);
    if (opened < 0) {
        return opened;
    }

    const auto file = static_cast<uv_file>(opened);
    if (uv_stat_t found{}; stat_file(file, found) == 0 && is_directory(found)) {
        close_file(file);
        return UV_EISDIR;
    }

    if (truncates) {
        if (const std::int64_t emptied = empty_file(file); emptied < 0) {
            close_file(file);
            return emptied;
        }
    }

    request.stream = new stream_t{ .file = file };
    return 0;
}

/// Reads up to `count` bytes, at `offset` in `file`, or from where it is for -1, into `data`: how
/// many, 0 at the end, or an error.
std::int64_t read_at(const uv_file file, char* data, const std::int64_t count,
                     const std::int64_t offset) {
    // Nothing is past the last place an Int64 counts, which the OS refuses to read up to.
    const std::int64_t most =
        offset < 0 ? count : std::min<std::int64_t>(count, INT64_MAX - offset);
    if (most <= 0) {
        return 0;
    }

    // libuv takes a buffer's size as an unsigned int.
    const std::size_t size = std::min<std::uint64_t>(static_cast<std::uint64_t>(most), 1U << 30U);
    const uv_buf_t buffer = uv_buf_init(data, static_cast<unsigned int>(size));
    fs_t call;
    return uv_fs_read(nullptr, call.get(), file, &buffer, 1, offset, nullptr);
}

/// Writes all of `bytes` to `file` at `offset`, or from where it is for -1, as an appending file
/// always writes at its end.
std::int64_t write_at(const uv_file file, std::string_view bytes, std::int64_t offset) {
    while (!bytes.empty()) {
        const std::size_t size = std::min<std::size_t>(bytes.size(), 1U << 30U);
        const uv_buf_t buffer =
            uv_buf_init(const_cast<char*>(bytes.data()), static_cast<unsigned int>(size));
        fs_t call;
        const int written = uv_fs_write(nullptr, call.get(), file, &buffer, 1, offset, nullptr);
        // A place the file system can't make a file reach is EINVAL on Windows and EFBIG on
        // POSIX, which is no fault of the path.
        if (written == UV_EINVAL && offset >= 0) {
            return UV_EFBIG;
        }

        if (written < 0) {
            return known_error(call.get(), written);
        }

        bytes.remove_prefix(static_cast<std::size_t>(written));
        if (offset >= 0) {
            offset += written;
        }
    }

    return 0;
}

/// A stream's Async read, into the request's bytes, which fb_io_request_copy gives.
std::int64_t stream_read(request_t& request) {
    buffer_t& bytes = request.bytes;
    bytes.clear();
    if (!bytes.reserve(static_cast<std::size_t>(request.number))) {
        return UV_ENOMEM;
    }

    const std::int64_t count = read_at(request.file, bytes.end(), request.number, request.offset);
    if (count > 0) {
        bytes.add(static_cast<std::size_t>(count));
    }

    return count;
}

std::int64_t stream_write(const request_t& request) {
    return write_at(request.file, request.input, request.offset);
}

std::int64_t sync_file(const uv_file file) {
    fs_t call;
    return uv_fs_fsync(nullptr, call.get(), file, nullptr);
}

std::int64_t stream_sync(const request_t& request) {
    return sync_file(request.file);
}

/// The work of a call that only reads its request.
template <std::int64_t (*Work)(const request_t&)>
std::int64_t reading(request_t& request) {
    return Work(request);
}

/// Runs a request's work on the calling thread.
std::int64_t run(request_t& request, const work_t work) {
    request.result = work(request);
    return request.result;
}

/// The I/O thread's end of an Async twin: the Task is done, with what the work gave.
void finish(request_t* request) {
    fb_task_t* task = request->task;
    // The Task's result, an Int64, is right after its fb_task_t, and it's done once that's set.
    *reinterpret_cast<std::int64_t*>(task + 1) = request->result;
    fb_task_finish(task);
    fb::runtime::finish_request(task);
    fb_object_release(&task->header);
    release(request);
}

void start_queued(uv_loop_t* loop, request_t* request) {
    request->queued.data = request;
    const int queued = uv_queue_work(
        loop, &request->queued,
        [](uv_work_t* work) {
            auto& self = *static_cast<request_t*>(work->data);
            self.result = self.work(self);
        },
        [](uv_work_t* work, const int status) {
            auto* self = static_cast<request_t*>(work->data);
            if (status != 0) {
                self->result = status;
            }

            finish(self);
        });
    if (queued != 0) {
        request->result = queued;
        finish(request);
    }
}

/// Has libuv's thread pool run a request's work, then finish `task` with what it gives. Once
/// the program is ending, the work runs here instead.
void run_async(fb_task_t* task, void* request, const work_t work) {
    auto* self = static_cast<request_t*>(request);
    self->work = work;
    self->task = task;
    // The I/O thread holds the request and the Task until it's done.
    self->owners.fetch_add(1, std::memory_order_relaxed);
    fb_object_retain(&task->header);
    fb::runtime::start_request(task);
    fb::runtime::on_io_thread([self](uv_loop_t* loop) {
        if (loop != nullptr) {
            start_queued(loop, self);
            return;
        }

        self->result = self->work(*self);
        finish(self);
    });
}

/// Runs a request's work on a thread of its own, then finishes `task` with what it gives, for
/// work that can wait as long as another program likes, like a lock, which would keep one of
/// libuv's few threads from every other Async call.
void run_on_thread(fb_task_t* task, void* request, const work_t work) {
    auto* self = static_cast<request_t*>(request);
    self->work = work;
    self->task = task;
    // The thread holds the request and the Task until it's done.
    self->owners.fetch_add(1, std::memory_order_relaxed);
    fb_object_retain(&task->header);
    fb::runtime::start_request(task);
    std::thread([self] {
        self->result = self->work(*self);
        finish(self);
    }).detach();
}

request_t& request_of(void* request) {
    return *static_cast<request_t*>(request);
}

/// Keeps the caller's bytes for a write, as a copy for an Async twin.
void keep_input(request_t& request, const std::string_view bytes, const bool copy) {
    if (copy) {
        request.owned.assign(bytes);
        request.input = request.owned;
    } else {
        request.input = bytes;
    }
}

std::string_view bytes_of(const fb_array_t* array) {
    return { static_cast<const char*>(array->data), static_cast<std::size_t>(array->count) };
}

/// `lines`, a String[], each with its line break after it.
std::string joined(const fb_array_t* lines, const bool crlf) {
    const std::string_view ending = crlf ? "\r\n" : "\n";
    const auto* const* texts = static_cast<const fb_string_t* const*>(lines->data);
    std::string text;
    for (std::int64_t index = 0; index < lines->count; ++index) {
        text += view_of(texts[index]);
        text += ending;
    }

    return text;
}

} // namespace

namespace fb::runtime {

atomic_file_t::~atomic_file_t() {
    discard();
}

std::int64_t atomic_file_t::open(const std::string& path) {
    discard();
    if (has_nul(path)) {
        return UV_EINVAL;
    }

    // An empty path names no file, which only the rename would find.
    if (path.empty()) {
        return UV_ENOENT;
    }

    m_path = linked_file(path);
    // A rename over a folder fails differently on each OS.
    if (uv_stat_t found{}; stat_path(m_path, true, found) == 0 && is_directory(found)) {
        return UV_EISDIR;
    }

    std::int64_t opened = UV_EEXIST;
    for (int tries = 0; opened == UV_EEXIST && tries < 100; ++tries) {
        m_temporary = temporary_beside(m_path);
        opened = open_file(m_temporary, UV_FS_O_WRONLY | UV_FS_O_CREAT | UV_FS_O_EXCL, 0666);
    }

    if (opened < 0) {
        m_temporary.clear();
        return opened;
    }

    m_file = static_cast<uv_file>(opened);
    return 0;
}

std::int64_t atomic_file_t::write(const std::string_view bytes) const {
    if (m_file < 0) {
        return UV_EBADF;
    }

    return write_all(m_file, bytes);
}

std::int64_t atomic_file_t::commit() {
    if (m_file < 0) {
        return UV_EBADF;
    }

    std::int64_t result = 0;
#ifndef _WIN32
    if (uv_stat_t replaced{}; stat_path(m_path, true, replaced) == 0) {
        fs_t call;
        uv_fs_fchmod(nullptr, call.get(), m_file, static_cast<int>(replaced.st_mode & 07777U),
                     nullptr);
    }
#endif
    {
        fs_t call;
        result = uv_fs_fsync(nullptr, call.get(), m_file, nullptr);
    }

    close_file(std::exchange(m_file, -1));
    if (result == 0) {
#ifdef _WIN32
        result = replace_file(m_temporary, m_path);
#else
        fs_t call;
        result = uv_fs_rename(nullptr, call.get(), m_temporary.c_str(), m_path.c_str(), nullptr);
#endif
    }

    if (result == 0) {
        m_temporary.clear();
    }

    discard();
    return result;
}

void atomic_file_t::discard() {
    if (m_file >= 0) {
        close_file(std::exchange(m_file, -1));
    }

    if (!m_temporary.empty()) {
        fs_t call;
        uv_fs_unlink(nullptr, call.get(), m_temporary.c_str(), nullptr);
        m_temporary.clear();
    }
}

} // namespace fb::runtime

extern "C" {

void* fb_io_request_new() {
    return new request_t;
}

void fb_io_request_free(void* request) {
    release(static_cast<request_t*>(request));
}

void fb_io_request_bytes(const void* request, fb_array_t* output) {
    const std::string_view bytes = static_cast<const request_t*>(request)->bytes.view();
    fb_array_reserve(output, 1, static_cast<std::int64_t>(bytes.size()), nullptr);
    output->count = static_cast<std::int64_t>(bytes.size());
    if (!bytes.empty()) {
        std::memcpy(output->data, bytes.data(), bytes.size());
    }
}

fb_string_t* fb_io_request_text(const void* request) {
    return string_of(static_cast<const request_t*>(request)->bytes.view());
}

void fb_io_request_lines(const void* request, fb_array_t* output) {
    std::string_view rest = static_cast<const request_t*>(request)->bytes.view();
    while (!rest.empty()) {
        const std::size_t end = rest.find('\n');
        std::string_view line = rest.substr(0, end);
        rest.remove_prefix(end == std::string_view::npos ? rest.size() : end + 1);
        if (end != std::string_view::npos && line.ends_with('\r')) {
            line.remove_suffix(1);
        }

        *static_cast<fb_string_t**>(fb_array_push(output, sizeof(fb_string_t*), nullptr)) =
            string_of(line);
    }
}

std::int64_t fb_io_request_count(const void* request) {
    return static_cast<std::int64_t>(static_cast<const request_t*>(request)->entries.size());
}

fb_string_t* fb_io_request_entry_path(const void* request, const std::int64_t index) {
    return string_of(
        static_cast<const request_t*>(request)->entries[static_cast<std::size_t>(index)].path);
}

std::int32_t fb_io_request_entry_kind(const void* request, const std::int64_t index) {
    return static_cast<const request_t*>(request)->entries[static_cast<std::size_t>(index)].kind;
}

void fb_io_request_stat(const void* request, fb_array_t* output) {
    const auto& self = *static_cast<const request_t*>(request);
    const uv_stat_t& stat = self.stat;
    const std::array<std::int64_t, 9> values{
        kind_of(stat),
        static_cast<std::int64_t>(stat.st_size),
        self.times[0].seconds,
        self.times[0].nanoseconds,
        self.times[1].seconds,
        self.times[1].nanoseconds,
        self.times[2].seconds,
        self.times[2].nanoseconds,
        (stat.st_mode & 0200U) == 0 ? 1 : 0,
    };
    constexpr auto count = static_cast<std::int64_t>(values.size());
    fb_array_reserve(output, sizeof(std::int64_t), count, nullptr);
    output->count = count;
    std::memcpy(output->data, values.data(), sizeof values);
}

fb_string_t* fb_io_request_failed(const void* request) {
    return string_of(static_cast<const request_t*>(request)->other);
}

fb_string_t* fb_io_request_path(const void* request) {
    return string_of(static_cast<const request_t*>(request)->path);
}

void* fb_io_request_lock(void* request) {
    return std::exchange(request_of(request).lock, nullptr);
}

std::int32_t fb_io_error_kind(const std::int64_t error) {
    switch (error) {
    case UV_ENOENT:
        return 1;
    case UV_EACCES:
    case UV_EPERM:
    case UV_EROFS:
        return 2;
    case UV_EEXIST:
        return 3;
    case UV_EINTR:
        return 4;
    case UV_ENOTDIR:
        return 5;
    case UV_EISDIR:
        return 6;
    case UV_ENOTEMPTY:
        return 7;
    case UV_EBUSY:
    case UV_ETXTBSY:
        return 8;
    case UV_ENOSPC:
    case UV_EFBIG:
        return 9;
    case UV_EINVAL:
    case UV_ENAMETOOLONG:
        return 10;
    default:
        return 0;
    }
}

fb_string_t* fb_io_error_message(const std::int64_t error) {
    if (error == too_big) {
        return string_of("it holds more bytes than the read takes");
    }

    return string_of(uv_strerror(static_cast<int>(error)));
}

std::int64_t fb_io_file_read(void* request, const fb_string_t* path, const std::int64_t most) {
    request_t& self = request_of(request);
    self.path = view_of(path);
    self.number = most;
    return run(self, read_file);
}

void fb_io_file_read_async(fb_task_t* task, void* request, const fb_string_t* path,
                           const std::int64_t most) {
    request_t& self = request_of(request);
    self.path = view_of(path);
    self.number = most;
    run_async(task, request, read_file);
}

std::int64_t fb_io_file_write(void* request, const fb_string_t* path, const fb_array_t* data,
                              const std::int32_t how) {
    request_t& self = request_of(request);
    self.path = view_of(path);
    self.number = how;
    keep_input(self, bytes_of(data), false);
    return run(self, reading<write_file>);
}

void fb_io_file_write_async(fb_task_t* task, void* request, const fb_string_t* path,
                            const fb_array_t* data, const std::int32_t how) {
    request_t& self = request_of(request);
    self.path = view_of(path);
    self.number = how;
    keep_input(self, bytes_of(data), true);
    run_async(task, request, reading<write_file>);
}

std::int64_t fb_io_file_write_text(void* request, const fb_string_t* path, const fb_string_t* text,
                                   const std::int32_t how) {
    request_t& self = request_of(request);
    self.path = view_of(path);
    self.number = how;
    keep_input(self, view_of(text), false);
    return run(self, reading<write_file>);
}

void fb_io_file_write_text_async(fb_task_t* task, void* request, const fb_string_t* path,
                                 const fb_string_t* text, const std::int32_t how) {
    request_t& self = request_of(request);
    self.path = view_of(path);
    self.number = how;
    keep_input(self, view_of(text), true);
    run_async(task, request, reading<write_file>);
}

std::int64_t fb_io_file_write_lines(void* request, const fb_string_t* path, const fb_array_t* lines,
                                    const std::int32_t crlf, const std::int32_t how) {
    request_t& self = request_of(request);
    self.path = view_of(path);
    self.number = how;
    self.owned = joined(lines, crlf != 0);
    self.input = self.owned;
    return run(self, reading<write_file>);
}

void fb_io_file_write_lines_async(fb_task_t* task, void* request, const fb_string_t* path,
                                  const fb_array_t* lines, const std::int32_t crlf,
                                  const std::int32_t how) {
    request_t& self = request_of(request);
    self.path = view_of(path);
    self.number = how;
    self.owned = joined(lines, crlf != 0);
    self.input = self.owned;
    run_async(task, request, reading<write_file>);
}

std::int64_t fb_io_file_stat(void* request, const fb_string_t* path, const std::int32_t follow) {
    request_t& self = request_of(request);
    self.path = view_of(path);
    self.flag = follow;
    return run(self, stat_request);
}

void fb_io_file_stat_async(fb_task_t* task, void* request, const fb_string_t* path,
                           const std::int32_t follow) {
    request_t& self = request_of(request);
    self.path = view_of(path);
    self.flag = follow;
    run_async(task, request, stat_request);
}

std::int64_t fb_io_file_copy(void* request, const fb_string_t* from, const fb_string_t* to) {
    request_t& self = request_of(request);
    self.path = view_of(from);
    self.other = view_of(to);
    return run(self, reading<copy_request>);
}

void fb_io_file_copy_async(fb_task_t* task, void* request, const fb_string_t* from,
                           const fb_string_t* to) {
    request_t& self = request_of(request);
    self.path = view_of(from);
    self.other = view_of(to);
    run_async(task, request, reading<copy_request>);
}

std::int64_t fb_io_file_move(void* request, const fb_string_t* from, const fb_string_t* to) {
    request_t& self = request_of(request);
    self.path = view_of(from);
    self.other = view_of(to);
    return run(self, reading<move_request>);
}

void fb_io_file_move_async(fb_task_t* task, void* request, const fb_string_t* from,
                           const fb_string_t* to) {
    request_t& self = request_of(request);
    self.path = view_of(from);
    self.other = view_of(to);
    run_async(task, request, reading<move_request>);
}

std::int64_t fb_io_file_delete(void* request, const fb_string_t* path) {
    request_of(request).path = view_of(path);
    return run(request_of(request), reading<delete_request>);
}

void fb_io_file_delete_async(fb_task_t* task, void* request, const fb_string_t* path) {
    request_of(request).path = view_of(path);
    run_async(task, request, reading<delete_request>);
}

std::int64_t fb_io_file_set_read_only(void* request, const fb_string_t* path,
                                      const std::int32_t read_only) {
    request_t& self = request_of(request);
    self.path = view_of(path);
    self.flag = read_only;
    return run(self, reading<set_read_only>);
}

void fb_io_file_set_read_only_async(fb_task_t* task, void* request, const fb_string_t* path,
                                    const std::int32_t read_only) {
    request_t& self = request_of(request);
    self.path = view_of(path);
    self.flag = read_only;
    run_async(task, request, reading<set_read_only>);
}

std::int64_t fb_io_dir_create(void* request, const fb_string_t* path, const std::int32_t all) {
    request_t& self = request_of(request);
    self.path = view_of(path);
    self.flag = all;
    return run(self, reading<create_request>);
}

void fb_io_dir_create_async(fb_task_t* task, void* request, const fb_string_t* path,
                            const std::int32_t all) {
    request_t& self = request_of(request);
    self.path = view_of(path);
    self.flag = all;
    run_async(task, request, reading<create_request>);
}

std::int64_t fb_io_dir_remove(void* request, const fb_string_t* path, const std::int32_t all) {
    request_t& self = request_of(request);
    self.path = view_of(path);
    self.flag = all;
    return run(self, reading<remove_request>);
}

void fb_io_dir_remove_async(fb_task_t* task, void* request, const fb_string_t* path,
                            const std::int32_t all) {
    request_t& self = request_of(request);
    self.path = view_of(path);
    self.flag = all;
    run_async(task, request, reading<remove_request>);
}

std::int64_t fb_io_dir_entries(void* request, const fb_string_t* path, const std::int64_t depth) {
    request_t& self = request_of(request);
    self.path = view_of(path);
    self.number = depth;
    return run(self, entries_request);
}

void fb_io_dir_entries_async(fb_task_t* task, void* request, const fb_string_t* path,
                             const std::int64_t depth) {
    request_t& self = request_of(request);
    self.path = view_of(path);
    self.number = depth;
    run_async(task, request, entries_request);
}

std::int64_t fb_io_temp_new(void* request, const fb_string_t* prefix, const std::int32_t folder) {
    request_t& self = request_of(request);
    const std::optional<std::string> temporary =
        path_from([](char* buffer, std::size_t* size) { return uv_os_tmpdir(buffer, size); });
    if (!temporary) {
        return UV_ENOENT;
    }

    const std::string pattern = trimmed(*temporary) + "/" + std::string(view_of(prefix)) + "XXXXXX";
    if (has_nul(pattern)) {
        return UV_EINVAL;
    }

    fs_t call;
    const int made = folder != 0 ? uv_fs_mkdtemp(nullptr, call.get(), pattern.c_str(), nullptr)
                                 : uv_fs_mkstemp(nullptr, call.get(), pattern.c_str(), nullptr);
    if (made < 0) {
        return made;
    }

    if (folder == 0) {
        close_file(made);
    }

    self.path = with_slashes(call.get()->path);
    return 0;
}

fb_string_t* fb_io_dir_current() {
    const std::optional<std::string> current =
        path_from([](char* buffer, std::size_t* size) { return uv_cwd(buffer, size); });
    return current ? string_of(*current) : nullptr;
}

fb_string_t* fb_io_dir_temp() {
    const std::optional<std::string> temporary =
        path_from([](char* buffer, std::size_t* size) { return uv_os_tmpdir(buffer, size); });
    return temporary ? string_of(trimmed(*temporary)) : nullptr;
}

fb_string_t* fb_io_real_path(const fb_string_t* path) {
    const std::string text(view_of(path));
    if (has_nul(text)) {
        return nullptr;
    }

    uv_fs_t call{};
    std::optional<std::string> real;
    if (uv_fs_realpath(nullptr, &call, text.c_str(), nullptr) == 0) {
        real = with_slashes(static_cast<const char*>(call.ptr));
    }

    uv_fs_req_cleanup(&call);
    return real ? string_of(*real) : nullptr;
}

fb_string_t* fb_io_dir_config() {
#ifdef _WIN32
    const std::optional<std::string> folder = path_from(
        [](char* buffer, std::size_t* size) { return uv_os_getenv("APPDATA", buffer, size); });
    if (!folder || folder->empty()) {
        return nullptr;
    }

    return string_of(trimmed(*folder));
#else
#ifndef __APPLE__
    // XDG's folder, when it's set to a path from the root, as the spec says it has to be.
    if (const std::optional<std::string> folder = path_from([](char* buffer, std::size_t* size) {
            return uv_os_getenv("XDG_CONFIG_HOME", buffer, size);
        });
        folder && folder->starts_with('/')) {
        return string_of(trimmed(*folder));
    }
#endif
    const std::optional<std::string> home =
        path_from([](char* buffer, std::size_t* size) { return uv_os_homedir(buffer, size); });
    if (!home || home->empty()) {
        return nullptr;
    }

#ifdef __APPLE__
    return string_of(trimmed(*home) + "/Library/Application Support");
#else
    return string_of(trimmed(*home) + "/.config");
#endif
#endif
}

std::int64_t fb_io_dir_set_current(void* request, const fb_string_t* path) {
    request_t& self = request_of(request);
    self.path = view_of(path);
    return has_nul(self.path) ? UV_EINVAL : uv_chdir(self.path.c_str());
}

std::int64_t fb_io_lock_acquire(void* request, const fb_string_t* path, const std::int32_t shared,
                                const std::int32_t wait) {
    request_t& self = request_of(request);
    self.path = view_of(path);
    self.flag = shared;
    self.number = wait;
    return run(self, acquire_lock);
}

void fb_io_lock_acquire_async(fb_task_t* task, void* request, const fb_string_t* path,
                              const std::int32_t shared, const std::int32_t wait) {
    request_t& self = request_of(request);
    self.path = view_of(path);
    self.flag = shared;
    self.number = wait;
    if (wait != 0) {
        run_on_thread(task, request, acquire_lock);
    } else {
        run_async(task, request, acquire_lock);
    }
}

std::int32_t fb_io_lock_held(const void* lock) {
    return static_cast<const lock_t*>(lock)->held ? 1 : 0;
}

void fb_io_lock_release(void* lock) {
    auto& self = *static_cast<lock_t*>(lock);
    if (!self.held) {
        return;
    }

    self.held = false;
#ifdef _WIN32
    OVERLAPPED overlapped{};
    overlapped.Offset = lock_offset_low;
    overlapped.OffsetHigh = lock_offset_high;
    UnlockFileEx(uv_get_osfhandle(self.file), 0, 1, 0, &overlapped);
#else
    flock(self.file, LOCK_UN);
#endif
    close_file(self.file);
}

void fb_io_lock_free(void* lock) {
    fb_io_lock_release(lock);
    delete static_cast<lock_t*>(lock);
}

std::int64_t fb_io_file_open(void* request, const fb_string_t* path, const std::int32_t how) {
    request_t& self = request_of(request);
    self.path = view_of(path);
    self.flag = how;
    return run(self, open_request);
}

void fb_io_file_open_async(fb_task_t* task, void* request, const fb_string_t* path,
                           const std::int32_t how) {
    request_t& self = request_of(request);
    self.path = view_of(path);
    self.flag = how;
    run_async(task, request, open_request);
}

void* fb_io_request_stream(void* request) {
    return std::exchange(request_of(request).stream, nullptr);
}

void fb_io_request_copy(const void* request, std::uint8_t* into) {
    const std::string_view bytes = static_cast<const request_t*>(request)->bytes.view();
    if (!bytes.empty()) {
        std::memcpy(into, bytes.data(), bytes.size());
    }
}

std::int32_t fb_io_stream_open(const void* stream) {
    return static_cast<const stream_t*>(stream)->open ? 1 : 0;
}

std::int64_t fb_io_stream_read(void* stream, std::uint8_t* data, const std::int64_t count,
                               const std::int64_t offset) {
    return read_at(static_cast<stream_t*>(stream)->file, reinterpret_cast<char*>(data), count,
                   offset);
}

void fb_io_stream_read_async(fb_task_t* task, void* request, void* stream, const std::int64_t count,
                             const std::int64_t offset) {
    request_t& self = request_of(request);
    self.file = static_cast<stream_t*>(stream)->file;
    self.number = count;
    self.offset = offset;
    run_async(task, request, stream_read);
}

std::int64_t fb_io_stream_write(void* stream, const std::uint8_t* data, const std::int64_t count,
                                const std::int64_t offset) {
    return write_at(static_cast<stream_t*>(stream)->file,
                    { reinterpret_cast<const char*>(data), static_cast<std::size_t>(count) },
                    offset);
}

void fb_io_stream_write_async(fb_task_t* task, void* request, void* stream,
                              const std::uint8_t* data, const std::int64_t count,
                              const std::int64_t offset) {
    request_t& self = request_of(request);
    self.file = static_cast<stream_t*>(stream)->file;
    self.offset = offset;
    keep_input(self, { reinterpret_cast<const char*>(data), static_cast<std::size_t>(count) },
               true);
    run_async(task, request, reading<stream_write>);
}

std::int64_t fb_io_stream_size(void* stream) {
    uv_stat_t stat{};
    const std::int64_t found = stat_file(static_cast<stream_t*>(stream)->file, stat);
    return found < 0 ? found : static_cast<std::int64_t>(stat.st_size);
}

std::int64_t fb_io_stream_sync(void* stream) {
    return sync_file(static_cast<stream_t*>(stream)->file);
}

void fb_io_stream_sync_async(fb_task_t* task, void* request, void* stream) {
    request_of(request).file = static_cast<stream_t*>(stream)->file;
    run_async(task, request, reading<stream_sync>);
}

void fb_io_stream_close(void* stream) {
    auto& self = *static_cast<stream_t*>(stream);
    if (self.open) {
        self.open = false;
        close_file(self.file);
    }
}

void fb_io_stream_free(void* stream) {
    fb_io_stream_close(stream);
    delete static_cast<stream_t*>(stream);
}
}
