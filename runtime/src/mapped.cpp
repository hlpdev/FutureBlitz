// std.io's MappedFile: a file's bytes mapped into memory, read-only or writable, through
// mmap on POSIX and a file mapping's view on Windows, opened through libuv for its UTF-8 paths.

#include "fb/files.hpp"
#include "fb/runtime.hpp"

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
/// has none of.
struct map_t {
    std::int64_t error = 0;
    uv_file file = -1;
    std::uint8_t* data = nullptr;
    std::int64_t size = 0;
    bool writable = false;
    bool open = false;
};

void close_file(const uv_file file) {
    uv_fs_t call{};
    uv_fs_close(nullptr, &call, file, nullptr);
    uv_fs_req_cleanup(&call);
}

/// Maps the open file's `size` bytes: 0, or the OS's error.
std::int64_t map_bytes(map_t& map) {
#ifdef _WIN32
    HANDLE mapping =
        CreateFileMappingW(uv_get_osfhandle(map.file), nullptr,
                           map.writable ? PAGE_READWRITE : PAGE_READONLY, 0, 0, nullptr);
    if (mapping == nullptr) {
        return uv_translate_sys_error(static_cast<int>(GetLastError()));
    }

    // The view keeps the mapping, which can go now.
    void* view = MapViewOfFile(mapping, map.writable ? FILE_MAP_WRITE : FILE_MAP_READ, 0, 0, 0);
    const DWORD error = GetLastError();
    CloseHandle(mapping);
    if (view == nullptr) {
        return uv_translate_sys_error(static_cast<int>(error));
    }
#else
    void* view = mmap(nullptr, static_cast<std::size_t>(map.size),
                      PROT_READ | (map.writable ? PROT_WRITE : 0), MAP_SHARED, map.file, 0);
    if (view == MAP_FAILED) {
        return uv_translate_sys_error(errno);
    }
#endif
    map.data = static_cast<std::uint8_t*>(view);
    return 0;
}

std::int64_t open_map(map_t& map, const std::string& path) {
    if (path.find('\0') != std::string::npos) {
        return UV_EINVAL;
    }

    uv_fs_t call{};
    const int opened = uv_fs_open(nullptr, &call, path.c_str(),
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
    const std::int64_t mapped = found < 0 ? found : (map.size > 0 ? map_bytes(map) : 0);
    if (mapped < 0) {
        close_file(map.file);
        map.size = 0;
        return mapped;
    }

    map.open = true;
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
    return map_of(map).data;
}

std::int64_t fb_io_map_size(const void* map) {
    return map_of(map).size;
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
    if (!self.open) {
        return;
    }

    self.open = false;
    if (self.data != nullptr) {
#ifdef _WIN32
        UnmapViewOfFile(self.data);
#else
        munmap(self.data, static_cast<std::size_t>(self.size));
#endif
    }

    self.data = nullptr;
    self.size = 0;
    close_file(self.file);
}

void fb_io_map_free(void* map) {
    fb_io_map_close(map);
    delete static_cast<map_t*>(map);
}
}
