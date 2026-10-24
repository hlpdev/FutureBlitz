// std.io's FileWatcher, over libuv's file events: the I/O thread watches a file or a folder and
// queues what happens to it, which the program takes in turn, waiting for it or not, or awaits.

#include "fb/files.hpp"
#include "fb/runtime.hpp"
#include "internal.hpp"
#include "io_internal.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <utility>
#include <uv.h>
#include <vector>

namespace {

/// What happened to a path, as std.io numbers FileEventKind's.
enum event_kind_t : std::int32_t {
    EVENT_CREATED = 0,
    EVENT_CHANGED = 1,
    EVENT_REMOVED = 2,
};

struct event_t {
    std::int32_t kind = EVENT_CHANGED;
    std::string path;
};

/// A watched file or folder. The program holds it, and so does the I/O thread while its handle
/// is open.
struct watcher_t {
    std::atomic<std::int32_t> owners = 1;
    std::string path;
    /// Whether it's a folder, whose events name what's in it, or a file, whose events are its own.
    bool folder = false;
    /// The I/O thread's handle, whether it's watching yet, and the names it knows are there, a
    /// folder's entries' and "" for itself, which only that thread touches.
    uv_fs_event_t handle{};
    bool open = false;
    std::set<std::string, std::less<>> known;

    std::mutex mutex;
    std::condition_variable changed;
    /// Set once the I/O thread has tried to start: 0, or libuv's error.
    bool started = false;
    std::int64_t error = 0;
    std::deque<event_t> events;
    /// The Tasks of the Awaits for the next event.
    std::vector<fb_task_t*> waiting;
    bool closed = false;
    /// The event the program took last.
    event_t taken;
};

void release(watcher_t* watcher) {
    if (watcher->owners.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        delete watcher;
    }
}

/// The watchers whose handles are open, which only the I/O thread touches.
std::vector<watcher_t*>& open_watchers() {
    static std::vector<watcher_t*> watchers;
    return watchers;
}

/// Finishes the Task of an Await for the next event: 1 when there's one, or 0 once it's closed.
void finish(fb_task_t* task, const std::int64_t result) {
    *reinterpret_cast<std::int64_t*>(task + 1) = result;
    fb_task_finish(task);
    fb::runtime::finish_request(task);
    fb_object_release(&task->header);
}

/// Marks the watcher closed, dropping the events it hasn't given, wakes what waits for one, and
/// finishes what awaits one.
void mark_closed(watcher_t& watcher) {
    std::vector<fb_task_t*> waiting;
    {
        const std::scoped_lock lock(watcher.mutex);
        if (watcher.closed) {
            return;
        }

        watcher.closed = true;
        watcher.events.clear();
        waiting.swap(watcher.waiting);
    }

    watcher.changed.notify_all();
    for (fb_task_t* task : waiting) {
        finish(task, 0);
    }
}

void on_closed(uv_handle_t* handle) {
    release(static_cast<watcher_t*>(handle->data));
}

/// Closes a watcher's handle, on the I/O thread.
void close_handle(watcher_t& watcher) {
    if (!watcher.open) {
        return;
    }

    watcher.open = false;
    std::erase(open_watchers(), &watcher);
    uv_fs_event_stop(&watcher.handle);
    uv_close(reinterpret_cast<uv_handle_t*>(&watcher.handle), on_closed);
}

/// As the I/O thread stops, every watcher is closed, so its handle doesn't keep the loop going.
void stop_watchers(uv_loop_t* /*loop*/) {
    for (watcher_t* watcher : std::vector(open_watchers())) {
        mark_closed(*watcher);
        close_handle(*watcher);
    }
}

bool exists(const std::string& path) {
    uv_fs_t call{};
    const int result = uv_fs_lstat(nullptr, &call, path.c_str(), nullptr);
    uv_fs_req_cleanup(&call);
    return result == 0;
}

/// The names in the folder at `path`, and "" for the folder itself.
std::set<std::string, std::less<>> names_in(const std::string& path) {
    std::set<std::string, std::less<>> names{ "" };
    uv_fs_t call{};
    if (uv_fs_scandir(nullptr, &call, path.c_str(), 0, nullptr) >= 0) {
        uv_dirent_t entry{};
        while (uv_fs_scandir_next(&call, &entry) != UV_EOF) {
            names.emplace(entry.name);
        }
    }

    uv_fs_req_cleanup(&call);
    return names;
}

/// What happened to a name libuv tells of, which OSes tell apart differently, as macOS calls a
/// new file that's written a change: one that's there now and wasn't came, one that was and isn't
/// went, and one that was and is changed. One that came and went before this is nothing.
void on_event(uv_fs_event_t* handle, const char* name, const int /*events*/, const int status) {
    auto& watcher = *static_cast<watcher_t*>(handle->data);
    if (status < 0) {
        return;
    }

    const std::string_view key = watcher.folder && name != nullptr ? name : "";
    event_t event{ .path = watcher.path };
    if (!key.empty()) {
        event.path += "/";
        event.path += key;
    }

    const bool there = exists(event.path);
    const auto known = watcher.known.find(key);
    if (there && known == watcher.known.end()) {
        event.kind = EVENT_CREATED;
        watcher.known.emplace(key);
    } else if (!there && known != watcher.known.end()) {
        event.kind = EVENT_REMOVED;
        watcher.known.erase(known);
    } else if (!there) {
        return;
    }

    std::vector<fb_task_t*> waiting;
    {
        const std::scoped_lock lock(watcher.mutex);
        if (watcher.closed) {
            return;
        }

        watcher.events.push_back(std::move(event));
        waiting.swap(watcher.waiting);
    }

    watcher.changed.notify_all();
    for (fb_task_t* task : waiting) {
        finish(task, 1);
    }
}

void start_watching(uv_loop_t* loop, void* context) {
    auto& watcher = *static_cast<watcher_t*>(context);
    fb::runtime::at_io_stop(stop_watchers);
    if (watcher.folder) {
        watcher.known = names_in(watcher.path);
    } else if (exists(watcher.path)) {
        watcher.known.emplace("");
    }

    std::int64_t error = uv_fs_event_init(loop, &watcher.handle);
    watcher.handle.data = &watcher;
    if (error == 0) {
        error = uv_fs_event_start(&watcher.handle, on_event, watcher.path.c_str(), 0);
        if (error == 0) {
            watcher.open = true;
            open_watchers().push_back(&watcher);
        } else {
            uv_close(reinterpret_cast<uv_handle_t*>(&watcher.handle), on_closed);
        }
    } else {
        release(&watcher);
    }

    {
        const std::scoped_lock lock(watcher.mutex);
        watcher.started = true;
        watcher.error = error;
    }

    watcher.changed.notify_all();
}

void stop_watching(uv_loop_t* /*loop*/, void* context) {
    auto* watcher = static_cast<watcher_t*>(context);
    close_handle(*watcher);
    release(watcher);
}

watcher_t& watcher_of(void* watcher) {
    return *static_cast<watcher_t*>(watcher);
}

} // namespace

extern "C" {

void* fb_io_watcher_new(const fb_string_t* path) {
    auto* watcher = new watcher_t;
    watcher->path = std::string(reinterpret_cast<const char*>(path + 1),
                                static_cast<std::size_t>(path->length));
    if (watcher->path.find('\0') != std::string::npos) {
        watcher->error = UV_EINVAL;
        return watcher;
    }

    // A folder's events name what's in it; a file's are the file's.
    uv_fs_t call{};
    if (uv_fs_stat(nullptr, &call, watcher->path.c_str(), nullptr) == 0) {
        watcher->folder = (call.statbuf.st_mode & S_IFMT) == S_IFDIR;
    }

    uv_fs_req_cleanup(&call);
    // The I/O thread holds it until its handle is closed.
    watcher->owners.store(2, std::memory_order_relaxed);
    fb::runtime::on_io_thread([watcher](uv_loop_t* loop) {
        if (loop != nullptr) {
            start_watching(loop, watcher);
            return;
        }

        // The program is ending, so the I/O thread doesn't take it.
        watcher->owners.store(1, std::memory_order_relaxed);
        const std::scoped_lock lock(watcher->mutex);
        watcher->started = true;
        watcher->error = UV_ECANCELED;
    });

    std::unique_lock lock(watcher->mutex);
    watcher->changed.wait(lock, [&] { return watcher->started; });
    return watcher;
}

std::int64_t fb_io_watcher_error(void* watcher) {
    const std::scoped_lock lock(watcher_of(watcher).mutex);
    return watcher_of(watcher).error;
}

std::int32_t fb_io_watcher_next(void* watcher, const std::int32_t wait) {
    watcher_t& self = watcher_of(watcher);
    std::unique_lock lock(self.mutex);
    if (wait != 0) {
        self.changed.wait(lock, [&] { return !self.events.empty() || self.closed; });
    }

    if (self.events.empty()) {
        return 0;
    }

    self.taken = std::move(self.events.front());
    self.events.pop_front();
    return 1;
}

void fb_io_watcher_wait_async(fb_task_t* task, void* watcher) {
    watcher_t& self = watcher_of(watcher);
    fb_object_retain(&task->header);
    fb::runtime::start_request(task);
    std::int64_t ready = -1;
    {
        const std::scoped_lock lock(self.mutex);
        if (!self.events.empty()) {
            ready = 1;
        } else if (self.closed) {
            ready = 0;
        } else {
            self.waiting.push_back(task);
        }
    }

    if (ready >= 0) {
        finish(task, ready);
    }
}

std::int32_t fb_io_watcher_kind(void* watcher) {
    return watcher_of(watcher).taken.kind;
}

fb_string_t* fb_io_watcher_path(void* watcher) {
    const std::string& path = watcher_of(watcher).taken.path;
    return fb_string_from_bytes(path.data(), static_cast<std::int64_t>(path.size()));
}

void fb_io_watcher_close(void* watcher) {
    watcher_t& self = watcher_of(watcher);
    {
        const std::scoped_lock lock(self.mutex);
        if (self.closed || !self.started || self.error != 0) {
            self.closed = true;
            self.events.clear();
            return;
        }
    }

    mark_closed(self);
    // The handle goes on the I/O thread, unless it's stopping, which closes it anyway.
    self.owners.fetch_add(1, std::memory_order_relaxed);
    fb::runtime::on_io_thread([watcher = &self](uv_loop_t* loop) {
        if (loop != nullptr) {
            stop_watching(loop, watcher);
        } else {
            release(watcher);
        }
    });
}

void fb_io_watcher_free(void* watcher) {
    fb_io_watcher_close(watcher);
    release(static_cast<watcher_t*>(watcher));
}
}
