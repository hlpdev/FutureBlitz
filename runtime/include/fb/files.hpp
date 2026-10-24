#pragma once

#include "fb/runtime.hpp"

#include <cstdint>

// std.io's files in the runtime, over libuv. Each call is given a request, which fb_io_request_new
// makes and fb_io_request_free gives back, and it puts what it gives there, for the
// fb_io_request_ functions to read once it's done. A call gives 0 or more when it worked, or one
// of libuv's errors, below 0, which fb_io_error_kind and fb_io_error_message tell about. Its Async
// twin, the same name with _async, is passed a new Task<Int64> first, which the I/O thread
// finishes with what the call would give, once libuv's thread pool has done it; the request
// keeps what it needs meanwhile, so the caller can let go of its arguments, and of the request.
//
// Paths are UTF-8 Strings, with '/' between names, and '\' too on Windows. One that holds a NUL is
// UV_EINVAL, since no file has it. What a call gives back as a path has '/' between names on every
// OS. Strings and arrays are only borrowed.
//
// An entry's kind is 0 a file, 1 a folder, 2 a link, or 3 something else. A write's `how` is 0 to
// make the file or replace what it held, 1 to add to its end, or 2 to write a temporary file beside
// it and rename that over it, which either replaces the file whole or leaves it as it was.

extern "C" {

/// 1 when the target's paths are Windows', and 0 when they're POSIX's, as std.io's Path reads
/// them. It's in the runtime itself, apart from libuv.
std::int32_t fb_path_windows();

void* fb_io_request_new();
/// Lets go of the request, which goes once a call under way with it is done too.
void fb_io_request_free(void* request);
/// The bytes a read gave, into `output`, a UInt8[], or as a String, with U+FFFD for each byte
/// that doesn't start well-formed UTF-8.
void fb_io_request_bytes(const void* request, fb_array_t* output);
fb_string_t* fb_io_request_text(const void* request);
/// The lines of what a read gave, split at "\n" and "\r\n", added to `output`, a String[]. A last
/// line with no line break after it is one too.
void fb_io_request_lines(const void* request, fb_array_t* output);
/// The entries a folder's listing gave: how many, and each one's path from the folder, with '/'
/// between names, and its kind.
std::int64_t fb_io_request_count(const void* request);
fb_string_t* fb_io_request_entry_path(const void* request, std::int64_t index);
std::int32_t fb_io_request_entry_kind(const void* request, std::int64_t index);
/// What fb_io_file_stat found, into `output`, an Int64[]: its kind, its size in bytes, when it was
/// changed, read, and made, in nanoseconds since 1970 began in UTC (0 for when it was made where
/// the OS doesn't say), and 1 when it's read-only or 0.
void fb_io_request_stat(const void* request, fb_array_t* output);
/// The path a temporary file or folder was made at, or a lock's, which fb_io_lock_release
/// gives back.
fb_string_t* fb_io_request_path(const void* request);
/// The folder a listing couldn't list, with '/' between names, or "" when there's none.
fb_string_t* fb_io_request_failed(const void* request);
void* fb_io_request_lock(void* request);

/// What std.io's IOErrorKind an error is: 0 OTHER, 1 NOT_FOUND, 2 PERMISSION_DENIED,
/// 3 ALREADY_EXISTS, 4 INTERRUPTED, 5 NOT_A_DIRECTORY, 6 IS_A_DIRECTORY, 7 NOT_EMPTY, 8 BUSY,
/// 9 NO_SPACE, or 10 INVALID_PATH; and what it says, like "no such file or directory".
std::int32_t fb_io_error_kind(std::int64_t error);
fb_string_t* fb_io_error_message(std::int64_t error);

/// Reads the whole file at `path`, and gives how many bytes it held, or -5000, which
/// fb_io_error_message tells of, when it holds more than `most`, which it finds before reading
/// them, where the OS knows a file's size. Memory the bytes don't fit in is UV_ENOMEM.
std::int64_t fb_io_file_read(void* request, const fb_string_t* path, std::int64_t most);
void fb_io_file_read_async(fb_task_t* task, void* request, const fb_string_t* path,
                           std::int64_t most);
/// Writes `data`, a UInt8[], `text`'s bytes, or `lines` (a String[]) each with "\n" after it, or
/// "\r\n" when `crlf` isn't 0, to the file at `path`, as `how` says.
std::int64_t fb_io_file_write(void* request, const fb_string_t* path, const fb_array_t* data,
                              std::int32_t how);
void fb_io_file_write_async(fb_task_t* task, void* request, const fb_string_t* path,
                            const fb_array_t* data, std::int32_t how);
std::int64_t fb_io_file_write_text(void* request, const fb_string_t* path, const fb_string_t* text,
                                   std::int32_t how);
void fb_io_file_write_text_async(fb_task_t* task, void* request, const fb_string_t* path,
                                 const fb_string_t* text, std::int32_t how);
std::int64_t fb_io_file_write_lines(void* request, const fb_string_t* path, const fb_array_t* lines,
                                    std::int32_t crlf, std::int32_t how);
void fb_io_file_write_lines_async(fb_task_t* task, void* request, const fb_string_t* path,
                                  const fb_array_t* lines, std::int32_t crlf, std::int32_t how);
/// Finds what's at `path`, through a link to what it links to unless `follow` is 0.
std::int64_t fb_io_file_stat(void* request, const fb_string_t* path, std::int32_t follow);
void fb_io_file_stat_async(fb_task_t* task, void* request, const fb_string_t* path,
                           std::int32_t follow);
/// Copies the file at `from` to `to`, replacing what's there.
std::int64_t fb_io_file_copy(void* request, const fb_string_t* from, const fb_string_t* to);
void fb_io_file_copy_async(fb_task_t* task, void* request, const fb_string_t* from,
                           const fb_string_t* to);
/// Moves the file or folder at `from` to `to`, replacing a file there; a file goes to another
/// disk as a copy, then the original's removed.
std::int64_t fb_io_file_move(void* request, const fb_string_t* from, const fb_string_t* to);
void fb_io_file_move_async(fb_task_t* task, void* request, const fb_string_t* from,
                           const fb_string_t* to);
/// Removes the file or link at `path`: 1, or 0 when there was none. A folder is UV_EISDIR.
std::int64_t fb_io_file_delete(void* request, const fb_string_t* path);
void fb_io_file_delete_async(fb_task_t* task, void* request, const fb_string_t* path);
/// Makes the file at `path` read-only, or writable for its owner when `read_only` is 0.
std::int64_t fb_io_file_set_read_only(void* request, const fb_string_t* path,
                                      std::int32_t read_only);
void fb_io_file_set_read_only_async(fb_task_t* task, void* request, const fb_string_t* path,
                                    std::int32_t read_only);

/// Makes the folder at `path`, and when `all` isn't 0, the folders it's in that aren't there
/// yet: 1, or 0 when `all` isn't 0 and it was there already.
std::int64_t fb_io_dir_create(void* request, const fb_string_t* path, std::int32_t all);
void fb_io_dir_create_async(fb_task_t* task, void* request, const fb_string_t* path,
                            std::int32_t all);
/// Removes the empty folder at `path`, or when `all` isn't 0, it and what's in it, without
/// following links: 1, or 0 when there was none. A file is UV_ENOTDIR.
std::int64_t fb_io_dir_remove(void* request, const fb_string_t* path, std::int32_t all);
void fb_io_dir_remove_async(fb_task_t* task, void* request, const fb_string_t* path,
                            std::int32_t all);
/// Lists the folder at `path`, and the folders in it to `depth` folders down, or all of them
/// when it's below 1: each folder's entries in the order of their names' bytes, each before
/// what's in it, without going into links. Gives how many there are.
std::int64_t fb_io_dir_entries(void* request, const fb_string_t* path, std::int64_t depth);
void fb_io_dir_entries_async(fb_task_t* task, void* request, const fb_string_t* path,
                             std::int64_t depth);
/// Makes a new, empty folder, or file when `folder` is 0, in the OS's temporary folder, named
/// `prefix` and six letters and digits, at fb_io_request_path.
std::int64_t fb_io_temp_new(void* request, const fb_string_t* prefix, std::int32_t folder);
/// The current folder, and the OS's temporary folder, or null when the OS doesn't say.
fb_string_t* fb_io_dir_current();
fb_string_t* fb_io_dir_temp();
/// Makes `path` the current folder.
std::int64_t fb_io_dir_set_current(void* request, const fb_string_t* path);

/// Locks the file at `path`, made when it isn't there, for this one, or shared with others when
/// `shared` isn't 0, waiting while another holds it unless `wait` is 0: 1, at fb_io_request_lock,
/// or 0 when it didn't wait. A lock leaves reading and writing the file to anyone, on every OS.
std::int64_t fb_io_lock_acquire(void* request, const fb_string_t* path, std::int32_t shared,
                                std::int32_t wait);
void fb_io_lock_acquire_async(fb_task_t* task, void* request, const fb_string_t* path,
                              std::int32_t shared, std::int32_t wait);
/// Whether a lock is held: 1 until fb_io_lock_release unlocks it, and 0 after. Releasing it
/// again does nothing, and fb_io_lock_free gives it back, unlocking it if it's held.
std::int32_t fb_io_lock_held(const void* lock);
void fb_io_lock_release(void* lock);
void fb_io_lock_free(void* lock);

/// The file at `path` mapped into memory whole, read-only, or writable when `writable` isn't 0,
/// which fb_io_map_free gives back. fb_io_map_error gives 0, or the error it couldn't map with.
void* fb_io_map_new(const fb_string_t* path, std::int32_t writable);
std::int64_t fb_io_map_error(const void* map);
/// Whether it's mapped: 1 until fb_io_map_close unmaps it, which a second close leaves alone.
std::int32_t fb_io_map_open(const void* map);
/// Where its bytes are, and how many there are: null and 0 once it's closed, and null for an
/// empty file.
std::uint8_t* fb_io_map_data(const void* map);
std::int64_t fb_io_map_size(const void* map);
/// Copies `count` bytes from `start` into `output`, a UInt8[], or `bytes`, a UInt8[], to `start`,
/// in an open one. std.io checks they're all in it, and that it's writable for a write.
void fb_io_map_read(const void* map, std::int64_t start, std::int64_t count, fb_array_t* output);
void fb_io_map_write(void* map, std::int64_t start, const fb_array_t* bytes);
/// Writes what was changed in a writable one onto the disk: 0, or an error.
std::int64_t fb_io_map_flush(void* map);
void fb_io_map_close(void* map);
void fb_io_map_free(void* map);

/// A watcher of the file or folder at `path`, which the I/O thread has started, and which
/// fb_io_watcher_free gives back. fb_io_watcher_error gives 0, or the error it couldn't start
/// with. An event is 0 created, 1 changed, or 2 removed, at a path: a folder's events are about
/// what's in it, and a file's about the file.
void* fb_io_watcher_new(const fb_string_t* path);
std::int64_t fb_io_watcher_error(void* watcher);
/// Takes the next event: 1, or 0 when there's none, which, unless `wait` is 0, is once it's
/// closed, after waiting for one until then.
std::int32_t fb_io_watcher_next(void* watcher, std::int32_t wait);
/// Finishes `task`, a new Task<Int64>, once there's an event to take, with 1, or once the
/// watcher's closed, with 0.
void fb_io_watcher_wait_async(fb_task_t* task, void* watcher);
/// The event taken last: its kind, and its path.
std::int32_t fb_io_watcher_kind(void* watcher);
fb_string_t* fb_io_watcher_path(void* watcher);
/// Stops watching, dropping the events not taken yet, which ends what waits for one.
void fb_io_watcher_close(void* watcher);
void fb_io_watcher_free(void* watcher);
}
