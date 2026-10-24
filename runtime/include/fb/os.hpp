#pragma once

#include "fb/runtime.hpp"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

// std.os's functions in the runtime, over libuv and, where it has nothing, the OS: the
// environment, the system, its standard folders, and opening a file or URL in its default app.
// Their prefix, fb_os_, links libuv with them. Strings are only borrowed, and a String they give
// is the caller's. A text that isn't there, or that the OS doesn't give, is null.
//
// The numbers std.os passes: a platform is 1 Windows, 2 Linux, or 3 macOS; an architecture 1 x64
// or 2 ARM64; and a folder 1 the executable's, 2 home, 3 config, 4 data, 5 cache, 6 temp, or
// 7 documents.

extern "C" {

/// The environment variable `name`'s value, or null when it isn't set. Any thread can use the
/// environment, though C code that reads it as another thread changes it may see either value.
fb_string_t* fb_os_env_get(const fb_string_t* name);
/// Sets `name` to `value`: 0, or 1 when `name` can't be a variable's (it's empty, or holds an `=`
/// or a NUL), or 2 when `value` holds a NUL.
std::int32_t fb_os_env_set(const fb_string_t* name, const fb_string_t* value);
/// Removes `name`: 0, or 1 when it can't be a variable's.
std::int32_t fb_os_env_unset(const fb_string_t* name);
/// Adds each variable's name to `names` and its value to `values`, both String[]s, in the OS's
/// order. Windows' hidden ones, whose names start with `=`, aren't among them.
void fb_os_env_all(fb_array_t* names, fb_array_t* values);

/// The machine's name on its network.
fb_string_t* fb_os_hostname();
/// The name of the user the program runs as.
fb_string_t* fb_os_user_name();
/// How many threads can run at once, 1 or more.
std::int32_t fb_os_cpu_count();
/// The machine's memory, in bytes.
std::int64_t fb_os_total_memory();
/// The platform and architecture the program was built for.
std::int32_t fb_os_platform();
std::int32_t fb_os_arch();

/// A standard folder's path, by its number.
fb_string_t* fb_os_folder(std::int32_t folder);

/// Opens `target`, a file, a folder, or a URL like "https://futureblitz.dev", in the app the OS
/// opens it with, and returns once the OS has taken it: 0, or 1 when there's no such file, 2 when
/// the program may not open it, or 3 when no app opens it.
std::int32_t fb_os_open_default(const fb_string_t* target);
}

// The paths std.os's folders are made from, apart from the OS, so the tests can check them on
// every platform.
namespace fb::os {

/// Keeps the program's own threads from reading the environment while one changes it: the
/// fb_os_env_ functions, and starting a process, which gets this program's.
[[nodiscard]] std::mutex& environment_lock();

/// Whether `target` starts with a URL's scheme and its colon, like "https:" or "mailto:". A
/// scheme of one letter is a Windows drive's, as in "C:".
[[nodiscard]] bool is_url(std::string_view target);

/// The folder an XDG variable's `value` names, when it's an absolute path, or else `fallback` in
/// `home`; nullopt for neither.
[[nodiscard]] std::optional<std::string> xdg_folder(const std::optional<std::string>& value,
                                                    const std::optional<std::string>& home,
                                                    std::string_view fallback);

/// The documents folder that the `text` of a user-dirs.dirs names, as xdg-user-dirs writes it:
/// `XDG_DOCUMENTS_DIR="$HOME/Documents"`, in `home`, or an absolute path. The last line that
/// names one counts, as when a shell reads the file, and nullopt for none.
[[nodiscard]] std::optional<std::string> user_dirs_documents(std::string_view text,
                                                             std::string_view home);

} // namespace fb::os
