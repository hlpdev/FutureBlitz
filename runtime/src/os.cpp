// std.os's functions over libuv: the environment, the system's names and sizes, and the home and
// temporary folders. Where libuv has nothing, the OS's own: the other standard folders, and
// opening a file or URL in its default app.

#include "fb/os.hpp"

#include "fb/runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <uv.h>

#ifdef _WIN32
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace {

/// What std.os's numbers stand for.
enum class folder_t : std::int8_t {
    EXECUTABLE = 1,
    HOME,
    CONFIG,
    DATA,
    CACHE,
    TEMP,
    DOCUMENTS,
};

enum class opened_t : std::int8_t {
    DONE,
    NOT_FOUND,
    DENIED,
    NO_APP,
};

/// Keeps the program's own threads from reading the environment while one changes it.
std::mutex& environment_lock() {
    static std::mutex lock;
    return lock;
}

std::string_view text_of(const fb_string_t* string) {
    return { reinterpret_cast<const char*>(string + 1), static_cast<std::size_t>(string->length) };
}

fb_string_t* string_of(const std::optional<std::string>& text) {
    return text ? fb_string_from_bytes(text->data(), static_cast<std::int64_t>(text->size()))
                : nullptr;
}

/// Whether `name` can be a variable's: not empty, and without an `=` or a NUL.
bool is_variable_name(const std::string_view name) {
    return !name.empty() && !name.contains('=') && !name.contains('\0');
}

/// What a libuv function that fills a buffer gives, with a bigger buffer when it asks for one,
/// or nullopt when it fails.
template <typename Fill>
std::optional<std::string> filled(Fill&& fill) {
    std::string buffer(256, '\0');
    std::size_t size = buffer.size();
    int result = fill(buffer.data(), &size);
    if (result == UV_ENOBUFS) {
        buffer.resize(size);
        result = fill(buffer.data(), &size);
    }

    if (result != 0) {
        return std::nullopt;
    }

    buffer.resize(size);
    return buffer;
}

std::optional<std::string> variable(const std::string& name) {
    return filled(
        [&](char* buffer, std::size_t* size) { return uv_os_getenv(name.c_str(), buffer, size); });
}

std::optional<std::string> home() {
    return filled(uv_os_homedir);
}

#ifdef _WIN32

std::wstring wide(const std::string_view text) {
    const int size =
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(),
                        size);
    return result;
}

std::string narrow(const std::wstring_view text) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size,
                        nullptr, nullptr);
    return result;
}

// The known folders' IDs, from knownfolders.h, here so that nothing links uuid.lib for them.
constexpr GUID roaming_app_data{
    .Data1 = 0x3EB685DB,
    .Data2 = 0x65F9,
    .Data3 = 0x4CF6,
    .Data4 = { 0xA0, 0x3A, 0xE3, 0xEF, 0x65, 0x72, 0x9F, 0x3D },
};
constexpr GUID local_app_data{
    .Data1 = 0xF1B32785,
    .Data2 = 0x6FBA,
    .Data3 = 0x4FCF,
    .Data4 = { 0x9D, 0x55, 0x7B, 0x8E, 0x7F, 0x15, 0x70, 0x91 },
};
constexpr GUID documents{
    .Data1 = 0xFDD39AD0,
    .Data2 = 0x238F,
    .Data3 = 0x46AF,
    .Data4 = { 0xAD, 0xB4, 0x6C, 0x85, 0x48, 0x03, 0x69, 0xC7 },
};

std::optional<std::string> known_folder(const GUID& id) {
    PWSTR path = nullptr;
    std::optional<std::string> found;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &path))) {
        found = narrow(path);
    }

    CoTaskMemFree(path);
    return found;
}

std::optional<std::string> standard_folder(const folder_t which) {
    switch (which) {
    case folder_t::CONFIG:
    case folder_t::DATA:
        return known_folder(roaming_app_data);
    case folder_t::CACHE:
        return known_folder(local_app_data);
    case folder_t::DOCUMENTS:
        return known_folder(documents);
    default:
        return std::nullopt;
    }
}

bool exists(const std::string_view path) {
    return GetFileAttributesW(wide(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

opened_t open_default(const std::string_view target) {
    // The shell can hand the target to a COM object, which wants COM started on the thread.
    const HRESULT started = CoInitializeEx(nullptr, static_cast<DWORD>(COINIT_APARTMENTTHREADED) |
                                                        static_cast<DWORD>(COINIT_DISABLE_OLE1DDE));
    const auto result = reinterpret_cast<std::intptr_t>(
        ShellExecuteW(nullptr, L"open", wide(target).c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (SUCCEEDED(started)) {
        CoUninitialize();
    }

    if (result > 32) {
        return opened_t::DONE;
    }

    switch (result) {
    case SE_ERR_FNF:
    case SE_ERR_PNF:
        return opened_t::NOT_FOUND;
    case SE_ERR_ACCESSDENIED:
        return opened_t::DENIED;
    default:
        return opened_t::NO_APP;
    }
}

#elifdef __APPLE__

std::optional<std::string> standard_folder(const folder_t which) {
    const std::optional<std::string> base = home();
    if (!base) {
        return std::nullopt;
    }

    switch (which) {
    case folder_t::CONFIG:
    case folder_t::DATA:
        return *base + "/Library/Application Support";
    case folder_t::CACHE:
        return *base + "/Library/Caches";
    case folder_t::DOCUMENTS:
        return *base + "/Documents";
    default:
        return std::nullopt;
    }
}

#else

std::optional<std::string> xdg_folder(const char* name, const std::string_view fallback) {
    return fb::os::xdg_folder(variable(name), home(), fallback);
}

/// The documents folder that ~/.config/user-dirs.dirs names, or else ~/Documents.
std::optional<std::string> documents() {
    const std::optional<std::string> base = home();
    const std::optional<std::string> config = xdg_folder("XDG_CONFIG_HOME", ".config");
    if (!base || !config) {
        return std::nullopt;
    }

    std::string text;
    std::FILE* file = std::fopen((*config + "/user-dirs.dirs").c_str(), "r");
    std::array<char, 4096> chunk{};
    std::size_t read = 0;
    while (file != nullptr && (read = std::fread(chunk.data(), 1, chunk.size(), file)) > 0) {
        text.append(chunk.data(), read);
    }

    if (file != nullptr) {
        std::fclose(file);
    }

    std::optional<std::string> found = fb::os::user_dirs_documents(text, *base);
    return found ? found : std::optional(*base + "/Documents");
}

std::optional<std::string> standard_folder(const folder_t which) {
    switch (which) {
    case folder_t::CONFIG:
        return xdg_folder("XDG_CONFIG_HOME", ".config");
    case folder_t::DATA:
        return xdg_folder("XDG_DATA_HOME", ".local/share");
    case folder_t::CACHE:
        return xdg_folder("XDG_CACHE_HOME", ".cache");
    case folder_t::DOCUMENTS:
        return documents();
    default:
        return std::nullopt;
    }
}

#endif

#ifndef _WIN32

bool exists(const std::string_view path) {
    struct stat found{};
    return stat(std::string(path).c_str(), &found) == 0;
}

/// Runs macOS's open or freedesktop's xdg-open with the target, quietly, and waits for it to
/// hand the target over.
opened_t open_default(const std::string_view target) {
#ifdef __APPLE__
    std::string opener = "open";
#else
    std::string opener = "xdg-open";
#endif
    std::string argument(target);
    std::array<char*, 3> arguments{ opener.data(), argument.data(), nullptr };
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
    pid_t child = 0;
    const int error =
        posix_spawnp(&child, opener.c_str(), &actions, nullptr, arguments.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (error != 0) {
        return opened_t::NO_APP;
    }

    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}

    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? opened_t::DONE : opened_t::NO_APP;
}

#endif

} // namespace

extern "C" {

fb_string_t* fb_os_env_get(const fb_string_t* name) {
    const std::string_view text = text_of(name);
    if (!is_variable_name(text)) {
        return nullptr;
    }

    const std::scoped_lock lock(environment_lock());
    return string_of(variable(std::string(text)));
}

std::int32_t fb_os_env_set(const fb_string_t* name, const fb_string_t* value) {
    const std::string_view text = text_of(name);
    if (!is_variable_name(text)) {
        return 1;
    }

    if (text_of(value).contains('\0')) {
        return 2;
    }

    const std::scoped_lock lock(environment_lock());
    uv_os_setenv(std::string(text).c_str(), std::string(text_of(value)).c_str());
    return 0;
}

std::int32_t fb_os_env_unset(const fb_string_t* name) {
    const std::string_view text = text_of(name);
    if (!is_variable_name(text)) {
        return 1;
    }

    const std::scoped_lock lock(environment_lock());
    uv_os_unsetenv(std::string(text).c_str());
    return 0;
}

void fb_os_env_all(fb_array_t* names, fb_array_t* values) {
    const std::scoped_lock lock(environment_lock());
    uv_env_item_t* items = nullptr;
    int count = 0;
    if (uv_os_environ(&items, &count) != 0) {
        return;
    }

    for (int i = 0; i < count; ++i) {
        const std::string_view name = items[i].name;
        if (name.starts_with('=')) {
            continue;
        }

        const std::string_view value = items[i].value;
        *static_cast<fb_string_t**>(fb_array_push(names, sizeof(fb_string_t*), nullptr)) =
            fb_string_from_bytes(name.data(), static_cast<std::int64_t>(name.size()));
        *static_cast<fb_string_t**>(fb_array_push(values, sizeof(fb_string_t*), nullptr)) =
            fb_string_from_bytes(value.data(), static_cast<std::int64_t>(value.size()));
    }

    uv_os_free_environ(items, count);
}

fb_string_t* fb_os_hostname() {
    return string_of(filled(uv_os_gethostname));
}

fb_string_t* fb_os_user_name() {
    uv_passwd_t user{};
    if (uv_os_get_passwd(&user) != 0) {
        return nullptr;
    }

    fb_string_t* name = fb_string_from_cstring(user.username);
    uv_os_free_passwd(&user);
    return name;
}

std::int32_t fb_os_cpu_count() {
    return static_cast<std::int32_t>(uv_available_parallelism());
}

std::int64_t fb_os_total_memory() {
    return static_cast<std::int64_t>(uv_get_total_memory());
}

std::int32_t fb_os_platform() {
#ifdef _WIN32
    return 1;
#elifdef __APPLE__
    return 3;
#else
    return 2;
#endif
}

std::int32_t fb_os_arch() {
#if defined(__aarch64__) || defined(_M_ARM64)
    return 2;
#else
    return 1;
#endif
}

fb_string_t* fb_os_folder(const std::int32_t folder) {
    switch (static_cast<folder_t>(folder)) {
    case folder_t::EXECUTABLE: {
        // libuv cuts a path that doesn't fit, so the buffer holds the longest one there is.
        std::string path(32768, '\0');
        std::size_t size = path.size();
        if (uv_exepath(path.data(), &size) != 0) {
            return nullptr;
        }

        path.resize(size);
        return string_of(path);
    }
    case folder_t::HOME:
        return string_of(home());
    case folder_t::TEMP:
        return string_of(filled(uv_os_tmpdir));
    default:
        return string_of(standard_folder(static_cast<folder_t>(folder)));
    }
}

std::int32_t fb_os_open_default(const fb_string_t* target) {
    const std::string_view text = text_of(target);
    if (text.contains('\0') || (!fb::os::is_url(text) && !exists(text))) {
        return static_cast<std::int32_t>(opened_t::NOT_FOUND);
    }

    return static_cast<std::int32_t>(open_default(text));
}
}
