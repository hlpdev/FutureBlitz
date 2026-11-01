// std.io's MappedFile: a file's bytes mapped into memory, read-only or writable, through
// mmap on POSIX and a file mapping's view on Windows, opened through libuv for its UTF-8 paths.

#include "fb/files.hpp"
#include "fb/runtime.hpp"
#include "files_internal.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <uv.h>

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
#include <sys/mman.h>
#endif

namespace {

/// A mapped file: the file, open until it's closed, and its bytes' memory, which an empty file
/// has none of. A read-only one's Buffers are a view of their own, copied as they're written,
/// made the first time one's asked for. The views and the file stay while a Buffer holds them,
/// though it's closed.
struct map_t {
    std::int64_t error = 0;
    uv_file file = -1;
    std::uint8_t* data = nullptr;
    std::uint8_t* copied = nullptr;
    std::int64_t size = 0;
    std::int64_t holds = 0;
    bool writable = false;
    bool open = false;
    bool mapped = false;
};

void close_file(const uv_file file) {
    uv_fs_t call{};
    uv_fs_close(nullptr, &call, file, nullptr);
    uv_fs_req_cleanup(&call);
}

/// How a view is made: shared with the file, read-only or writable, or a copy of its pages made
/// as they're written.
enum class view_t : std::uint8_t {
    READ,
    WRITE,
    COPY,
};

/// A view of the open file's `size` bytes, into `view`: 0, or the OS's error.
std::int64_t map_view(const map_t& map, const view_t how, std::uint8_t*& view) {
#ifdef _WIN32
    DWORD protection = PAGE_READONLY;
    DWORD access = FILE_MAP_READ;
    if (how == view_t::WRITE) {
        protection = PAGE_READWRITE;
        access = FILE_MAP_WRITE;
    } else if (how == view_t::COPY) {
        protection = PAGE_WRITECOPY;
        access = FILE_MAP_COPY;
    }

    HANDLE mapping =
        CreateFileMappingW(uv_get_osfhandle(map.file), nullptr, protection, 0, 0, nullptr);
    if (mapping == nullptr) {
        return uv_translate_sys_error(static_cast<int>(GetLastError()));
    }

    // The view keeps the mapping, which can go now.
    void* made = MapViewOfFile(mapping, access, 0, 0, 0);
    const DWORD error = GetLastError();
    CloseHandle(mapping);
    if (made == nullptr) {
        return uv_translate_sys_error(static_cast<int>(error));
    }
#else
    const int protection = how == view_t::READ ? PROT_READ : PROT_READ | PROT_WRITE;
    void* made = mmap(nullptr, static_cast<std::size_t>(map.size), protection,
                      how == view_t::COPY ? MAP_PRIVATE : MAP_SHARED, map.file, 0);
    if (made == MAP_FAILED) {
        return uv_translate_sys_error(errno);
    }
#endif
    view = static_cast<std::uint8_t*>(made);
    return 0;
}

void unmap_view(std::uint8_t* view, [[maybe_unused]] const std::int64_t size) {
    if (view == nullptr) {
        return;
    }

#ifdef _WIN32
    UnmapViewOfFile(view);
#else
    munmap(view, static_cast<std::size_t>(size));
#endif
}

/// Unmaps the views and closes the file, once it's closed and no Buffer holds them.
void unmap(map_t& map) {
    if (map.open || map.holds > 0 || !map.mapped) {
        return;
    }

    map.mapped = false;
    unmap_view(map.data, map.size);
    unmap_view(map.copied, map.size);
    map.data = nullptr;
    map.copied = nullptr;
    close_file(map.file);
}

std::int64_t open_map(map_t& map, const std::string& path) {
    if (path.contains('\0')) {
        return UV_EINVAL;
    }

    uv_fs_t call{};
    const int opened = uv_fs_open(nullptr, &call, fb::runtime::os_path(path).c_str(),
                                  map.writable ? UV_FS_O_RDWR : UV_FS_O_RDONLY, 0, nullptr);
    uv_fs_req_cleanup(&call);
    if (opened < 0) {
        return opened;
    }

    map.file = opened;
    int found = uv_fs_fstat(nullptr, &call, map.file, nullptr);
    const uv_stat_t stat = call.statbuf;
    uv_fs_req_cleanup(&call);
    if (found == 0 && (stat.st_mode & S_IFMT) == S_IFDIR) {
        found = UV_EISDIR;
    }

    map.size = static_cast<std::int64_t>(stat.st_size);
    // An empty file has no bytes to map.
    std::int64_t mapped = found;
    if (mapped == 0 && map.size > 0) {
        mapped = map_view(map, map.writable ? view_t::WRITE : view_t::READ, map.data);
    }

    if (mapped < 0) {
        close_file(map.file);
        map.size = 0;
        return mapped;
    }

    map.open = true;
    map.mapped = true;
    return 0;
}

map_t& map_of(void* map) {
    return *static_cast<map_t*>(map);
}

const map_t& map_of(const void* map) {
    return *static_cast<const map_t*>(map);
}

} // namespace

extern "C" {

void* fb_io_map_new(const fb_string_t* path, const std::int32_t writable) {
    auto* map = new map_t{ .writable = writable != 0 };
    map->error = open_map(*map, std::string(reinterpret_cast<const char*>(path + 1),
                                            static_cast<std::size_t>(path->length)));
    return map;
}

std::int64_t fb_io_map_error(const void* map) {
    return map_of(map).error;
}

std::int32_t fb_io_map_open(const void* map) {
    return map_of(map).open ? 1 : 0;
}

std::uint8_t* fb_io_map_data(const void* map) {
    const map_t& self = map_of(map);
    return self.open ? self.data : nullptr;
}

std::int64_t fb_io_map_size(const void* map) {
    const map_t& self = map_of(map);
    return self.open ? self.size : 0;
}

std::int64_t fb_io_map_hold(void* map) {
    map_t& self = map_of(map);
    if (!self.writable && self.copied == nullptr && self.size > 0) {
        if (const std::int64_t mapped = map_view(self, view_t::COPY, self.copied); mapped < 0) {
            return mapped;
        }
    }

    ++self.holds;
    return 0;
}

std::uint8_t* fb_io_map_held(const void* map) {
    const map_t& self = map_of(map);
    return self.writable ? self.data : self.copied;
}

void fb_io_map_drop(void* map) {
    map_t& self = map_of(map);
    --self.holds;
    unmap(self);
}

void fb_io_map_read(const void* map, const std::int64_t start, const std::int64_t count,
                    fb_array_t* output) {
    fb_array_reserve(output, 1, count, nullptr);
    output->count = count;
    if (count > 0) {
        std::memcpy(output->data, map_of(map).data + start, static_cast<std::size_t>(count));
    }
}

void fb_io_map_write(void* map, const std::int64_t start, const fb_array_t* bytes) {
    if (bytes->count > 0) {
        std::memcpy(map_of(map).data + start, bytes->data, static_cast<std::size_t>(bytes->count));
    }
}

std::int64_t fb_io_map_flush(void* map) {
    const map_t& self = map_of(map);
    if (!self.open || !self.writable || self.data == nullptr) {
        return 0;
    }

#ifdef _WIN32
    if (FlushViewOfFile(self.data, 0) == 0 || FlushFileBuffers(uv_get_osfhandle(self.file)) == 0) {
        return uv_translate_sys_error(static_cast<int>(GetLastError()));
    }
#else
    if (msync(self.data, static_cast<std::size_t>(self.size), MS_SYNC) != 0) {
        return uv_translate_sys_error(errno);
    }
#endif
    return 0;
}

void fb_io_map_close(void* map) {
    map_t& self = map_of(map);
    self.open = false;
    unmap(self);
}

void fb_io_map_free(void* map) {
    fb_io_map_close(map);
    delete static_cast<map_t*>(map);
}
}
