// std.io's FileWatcher, over libuv's file events: the I/O thread watches a file or a folder, and
// for a recursive one the folders in it, and queues what happens to it, which the program takes
// in turn, waiting for it or not, or awaits.

#include "fb/files.hpp"
#include "fb/runtime.hpp"
#include "internal.hpp"
#include "io_internal.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iterator>
#include <map>
#include <mutex>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <utility>
#include <uv.h>
#include <vector>

namespace {

/// What happened to a path, as std.io numbers FileEventKind's.
enum event_kind_t : std::uint8_t {
    EVENT_CREATED = 0,
    EVENT_CHANGED = 1,
    EVENT_REMOVED = 2,
};

struct event_t {
    std::int32_t kind = EVENT_CHANGED;
    std::string path;
};

struct watcher_t;

#ifdef __linux__
/// A folder in a recursive watcher's folder, which inotify watches apart, since it watches one
/// folder at a time. Its handle holds the watcher.
struct subfolder_t {
    uv_fs_event_t handle{};
    watcher_t* watcher = nullptr;
    /// Where it is in the watcher's folder, as a key.
    std::string key;
};
#endif

/// A watched file or folder. The program holds it, and so does the I/O thread while its handle
/// is open.
struct watcher_t {
    /// Set once the I/O thread has tried to start: 0, or libuv's error.
    std::int64_t error = 0;
    /// The keys the I/O thread knows are there, "" for the folder itself and a name for each of
    /// its entries, with the entries of a recursive one's folders as their paths from it, with
    /// '/'s, like "art/hero.png", and the last listing of a folder in it, by its key, and the
    /// loop's time it was taken at, which only that thread touches.
    std::set<std::string, std::less<>> known;
    std::set<std::string, std::less<>> listing;
    std::string listed_folder;
    std::uint64_t listed_at = 0;
    /// The Tasks of the Awaits for the next event.
    std::vector<fb_task_t*> waiting;
    /// The path the program gave, which events' paths start with, and where it was when the
    /// watcher was made, which looking at the disk goes by, so a change of the current folder
    /// doesn't move it.
    std::string path;
    std::string real;
    std::deque<event_t> events;
    /// The event the program took last.
    event_t taken;
    std::condition_variable changed;
    std::mutex mutex;
    /// The I/O thread's handle, which only that thread touches.
    uv_fs_event_t handle{};
#ifdef __linux__
    /// A recursive watcher's folders' own handles, by their keys, and the first error one
    /// couldn't be watched with for inotify's sake, as when it has as many watches as Linux
    /// allows, which only the I/O thread touches.
    std::map<std::string, subfolder_t*, std::less<>> subfolders;
    std::int64_t unwatched = 0;
#endif
    std::atomic<std::int32_t> owners = 1;
    /// Whether it's a folder, whose events name what's in it, or a file, whose events are its own.
    bool folder = false;
    /// Whether it's a folder that's watched with the folders in it, and theirs.
    bool recursive = false;
    /// Whether the handle's watching, which only the I/O thread touches.
    bool open = false;
    bool started = false;
    bool closed = false;

    /// libuv's callbacks for the handle: what happened to a name, and the handle's gone.
    static void on_event(uv_fs_event_t* handle, const char* name, int events, int status);
    static void on_closed(uv_handle_t* handle);
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

void watcher_t::on_closed(uv_handle_t* handle) {
    release(static_cast<watcher_t*>(handle->data));
}

/// `folder` and `key`, with a '/' between them unless the folder ends with a separator, as
/// Path's Join puts them.
std::string joined(std::string folder, const std::string_view key) {
#ifdef _WIN32
    const bool separated = folder.ends_with('/') || folder.ends_with('\\');
#else
    const bool separated = folder.ends_with('/');
#endif
    if (!separated) {
        folder += '/';
    }

    folder += key;
    return folder;
}

/// The key of `name` in the folder at `parent`, a key itself, "" for the watched folder.
std::string key_of(const std::string_view parent, const std::string_view name) {
    std::string key(parent);
    if (!key.empty()) {
        key += '/';
    }

    key += name;
    return key;
}

/// The key of the folder a key's in, and its name there.
std::pair<std::string_view, std::string_view> split(const std::string_view key) {
    const std::size_t slash = key.rfind('/');
    if (slash == std::string_view::npos) {
        return { {}, key };
    }

    return { key.substr(0, slash), key.substr(slash + 1) };
}

/// The path of what's at `key` in a folder watcher's folder, from the path the program gave.
std::string path_in(const watcher_t& watcher, const std::string_view key) {
    return joined(watcher.path, key);
}

/// Where what's at `key` in a folder watcher's folder is on the disk.
std::string real_in(const watcher_t& watcher, const std::string_view key) {
    return key.empty() ? watcher.real : joined(watcher.real, key);
}

bool exists(const std::string& path) {
    uv_fs_t call{};
    const int result = uv_fs_lstat(nullptr, &call, path.c_str(), nullptr);
    uv_fs_req_cleanup(&call);
    return result == 0;
}

/// Whether what's at `path` is a folder, and not a link to one.
bool is_folder(const std::string& path) {
    uv_fs_t call{};
    const bool folder = uv_fs_lstat(nullptr, &call, path.c_str(), nullptr) == 0 &&
                        (call.statbuf.st_mode & S_IFMT) == S_IFDIR;
    uv_fs_req_cleanup(&call);
    return folder;
}

/// The names in the folder at `path`, and "" for the folder itself. With `folders`, the names of
/// the folders among them, and not the links to folders, go there too.
std::set<std::string, std::less<>> names_in(const std::string& path,
                                            std::vector<std::string>* folders = nullptr) {
    std::set<std::string, std::less<>> names{ "" };
    uv_fs_t call{};
    if (uv_fs_scandir(nullptr, &call, path.c_str(), 0, nullptr) >= 0) {
        uv_dirent_t entry{};
        while (uv_fs_scandir_next(&call, &entry) != UV_EOF) {
            names.emplace(entry.name);
            if (folders != nullptr &&
                (entry.type == UV_DIRENT_DIR ||
                 (entry.type == UV_DIRENT_UNKNOWN && is_folder(joined(path, entry.name))))) {
                folders->emplace_back(entry.name);
            }
        }
    }

    uv_fs_req_cleanup(&call);
    return names;
}

/// The keys of what's in a folder watcher's folder now, and of what's in the folders in it for a
/// recursive one, not through links, and "" for the folder itself.
std::set<std::string, std::less<>> listing_of(const watcher_t& watcher) {
    if (!watcher.recursive) {
        return names_in(watcher.real);
    }

    std::set<std::string, std::less<>> keys;
    std::vector<std::string> pending{ "" };
    while (!pending.empty()) {
        const std::string folder = std::move(pending.back());
        pending.pop_back();
        std::vector<std::string> inside;
        for (const std::string& name : names_in(real_in(watcher, folder), &inside)) {
            keys.emplace(name.empty() ? folder : key_of(folder, name));
        }

        for (const std::string& name : inside) {
            pending.push_back(key_of(folder, name));
        }
    }

    return keys;
}

/// Queues the events, unless the watcher's closed, and wakes what waits for one.
void queue(watcher_t& watcher, std::vector<event_t> events) {
    if (events.empty()) {
        return;
    }

    std::vector<fb_task_t*> waiting;
    {
        const std::scoped_lock lock(watcher.mutex);
        if (watcher.closed) {
            return;
        }

        for (event_t& event : events) {
            watcher.events.push_back(std::move(event));
        }

        waiting.swap(watcher.waiting);
    }

    watcher.changed.notify_all();
    for (fb_task_t* task : waiting) {
        finish(task, 1);
    }
}

#ifdef __linux__
void on_subfolder_event(uv_fs_event_t* handle, const char* name, int events, int status);

void on_subfolder_closed(uv_handle_t* handle) {
    auto* subfolder = static_cast<subfolder_t*>(handle->data);
    watcher_t* watcher = subfolder->watcher;
    delete subfolder;
    release(watcher);
}

/// Watches the folder at `key` in a recursive watcher's folder with a handle of its own, on the
/// watcher's loop, unless it has one.
void watch_subfolder(watcher_t& watcher, const std::string& key) {
    if (key.empty() || watcher.subfolders.contains(key)) {
        return;
    }

    auto* subfolder = new subfolder_t{ .watcher = &watcher, .key = key };
    uv_fs_event_init(watcher.handle.loop, &subfolder->handle);
    subfolder->handle.data = subfolder;
    watcher.owners.fetch_add(1, std::memory_order_relaxed);
    const int error =
        uv_fs_event_start(&subfolder->handle, on_subfolder_event, real_in(watcher, key).c_str(), 0);
    if (error != 0) {
        // One that went already is told of by its parent's handle, and one the program can't
        // read has nothing to tell of, but one past inotify's limits would go untold of.
        if ((error == UV_ENOSPC || error == UV_ENOMEM) && watcher.unwatched == 0) {
            watcher.unwatched = error;
        }

        uv_close(reinterpret_cast<uv_handle_t*>(&subfolder->handle), on_subfolder_closed);
        return;
    }

    watcher.subfolders.emplace(key, subfolder);
}

/// Stops watching the folder at `key` and the folders in it with their own handles.
void unwatch_subfolders(watcher_t& watcher, const std::string_view key) {
    const std::string inside = key.empty() ? std::string() : std::string(key) + '/';
    auto at = watcher.subfolders.lower_bound(key);
    while (at != watcher.subfolders.end() && (at->first == key || at->first.starts_with(inside))) {
        uv_fs_event_stop(&at->second->handle);
        uv_close(reinterpret_cast<uv_handle_t*>(&at->second->handle), on_subfolder_closed);
        at = watcher.subfolders.erase(at);
    }
}
#endif

/// Closes a watcher's handles, on the I/O thread.
void close_handle(watcher_t& watcher) {
    if (!watcher.open) {
        return;
    }

    watcher.open = false;
    std::erase(open_watchers(), &watcher);
#ifdef __linux__
    unwatch_subfolders(watcher, "");
#endif
    uv_fs_event_stop(&watcher.handle);
    uv_close(reinterpret_cast<uv_handle_t*>(&watcher.handle), watcher_t::on_closed);
}

#ifdef __APPLE__
/// libuv starts macOS's FSEvents a moment after a folder watcher's made, on a thread of its own,
/// and starts its one stream for every folder again for each new one, so what changes meanwhile
/// goes untold. So after a folder watcher's made, each lists its folder again a few times, which
/// finds what came and went. The timer and its step are the I/O thread's.
struct rescans_t {
    uv_timer_t timer{};
    bool made = false;
    std::size_t step = 0;
};

rescans_t& rescans() {
    static rescans_t instance;
    return instance;
}

/// The milliseconds before each listing again, after the last.
constexpr std::array<std::uint64_t, 4> rescan_waits{ 50, 200, 750, 3000 };

void stop_rescans() {
    rescans_t& state = rescans();
    if (state.made) {
        uv_timer_stop(&state.timer);
        uv_close(reinterpret_cast<uv_handle_t*>(&state.timer), nullptr);
        state.made = false;
    }
}
#endif

/// As the I/O thread stops, every watcher is closed, so its handle doesn't keep the loop going.
void stop_watchers(uv_loop_t* /*loop*/) {
#ifdef __APPLE__
    stop_rescans();
#endif
    for (watcher_t* watcher : std::vector(open_watchers())) {
        mark_closed(*watcher);
        close_handle(*watcher);
    }
}

/// Learns what's in the folder at `key` in a recursive watcher's folder, and in the folders in
/// it, as what came, each a CREATED in `found` unless it's null, the folders before what's in
/// them. On Linux, each of those folders is watched before it's listed, so what comes after is
/// told of.
void adopt(watcher_t& watcher, const std::string& key, std::vector<event_t>* found) {
    std::vector<std::string> pending{ key };
    while (!pending.empty()) {
        const std::string folder = std::move(pending.back());
        pending.pop_back();
#ifdef __linux__
        watch_subfolder(watcher, folder);
#endif
        std::vector<std::string> inside;
        for (const std::string& name : names_in(real_in(watcher, folder), &inside)) {
            const std::string entry = key_of(folder, name);
            if (!name.empty() && watcher.known.emplace(entry).second && found != nullptr) {
                found->push_back({ .kind = EVENT_CREATED, .path = path_in(watcher, entry) });
            }
        }

        // The last is listed first, so the ones listed before come first.
        for (const std::string& name : std::views::reverse(inside)) {
            pending.push_back(key_of(folder, name));
        }
    }
}

/// Forgets what a recursive watcher knew in the folder at `key`, which went, each a REMOVED in
/// `found`, what's in a folder before it, and on Linux stops watching the folders there.
void forget(watcher_t& watcher, const std::string_view key, std::vector<event_t>& found) {
    const std::string inside = std::string(key) + '/';
    const auto first = watcher.known.lower_bound(inside);
    auto last = first;
    while (last != watcher.known.end() && last->starts_with(inside)) {
        ++last;
    }

    for (auto at = std::make_reverse_iterator(last); at != std::make_reverse_iterator(first);
         ++at) {
        found.push_back({ .kind = EVENT_REMOVED, .path = path_in(watcher, *at) });
    }

    watcher.known.erase(first, last);
#ifdef __linux__
    unwatch_subfolders(watcher, key);
#endif
}

/// Lists a folder watcher's folder again and gives what came and went since the keys it knew,
/// as when libuv drops events it had no room for and says only that something changed: what
/// went, what's in a folder before it, then what came, a folder before what's in it.
std::vector<event_t> rescanned(watcher_t& watcher) {
    std::set<std::string, std::less<>> keys = listing_of(watcher);
    std::vector<event_t> events;
    for (const std::string& key : std::views::reverse(watcher.known)) {
        if (!key.empty() && !keys.contains(key)) {
            events.push_back({ .kind = EVENT_REMOVED, .path = path_in(watcher, key) });
#ifdef __linux__
            unwatch_subfolders(watcher, key);
#endif
        }
    }

    for (const std::string& key : keys) {
        if (!key.empty() && !watcher.known.contains(key)) {
            events.push_back({ .kind = EVENT_CREATED, .path = path_in(watcher, key) });
        }
    }

    watcher.known = std::move(keys);
#ifdef __linux__
    if (watcher.recursive) {
        for (const std::string& key : watcher.known) {
            if (!key.empty() && is_folder(real_in(watcher, key))) {
                watch_subfolder(watcher, key);
            }
        }
    }
#endif
    return events;
}

/// Whether a folder watcher's folder holds `key`, spelled as it is, by a listing of the folder
/// it's in, taken once for each turn of the loop, as libuv gives a batch of events in one, and
/// again for a name it lacks, which may have come since: in a burst, the files made after it are
/// told of in the same turn.
bool listed(watcher_t& watcher, const uv_loop_t* loop, const std::string_view key) {
    const auto [folder, name] = split(key);
    const std::uint64_t now = uv_now(loop) + 1;
    if (watcher.listed_at == now && watcher.listed_folder == folder &&
        watcher.listing.contains(name)) {
        return true;
    }

    watcher.listing = names_in(real_in(watcher, folder));
    watcher.listed_folder = folder;
    watcher.listed_at = now;
    return watcher.listing.contains(name);
}

/// What happened to `key`, which libuv tells of, which OSes tell apart differently, as macOS
/// calls a new file that's written a change: one that's there now and wasn't came, one that was
/// and isn't went, and one that was and is changed. One that came and went before this is
/// nothing. In a recursive watcher, a folder that came brings what's in it, and one that went
/// takes it.
void told(watcher_t& watcher, const uv_loop_t* loop, const std::string& key, const int events) {
    event_t event{ .path = watcher.folder ? path_in(watcher, key) : watcher.path };
    bool there = exists(watcher.folder ? real_in(watcher, key) : watcher.real);
    const auto known = watcher.known.find(key);
    // A disk that ignores case, as Windows' and macOS's usually do, finds a name by another
    // spelling too, as a rename that changed its case leaves it, so a name that would come, or a
    // known one libuv says was renamed, is looked for as it's spelled.
    if (there && watcher.folder &&
        (known == watcher.known.end() ||
         (static_cast<unsigned>(events) & static_cast<unsigned>(UV_RENAME)) != 0U)) {
        there = listed(watcher, loop, key);
    }

    std::vector<event_t> found;
    if (there && known == watcher.known.end()) {
        event.kind = EVENT_CREATED;
        watcher.known.emplace(key);
        found.push_back(std::move(event));
        if (watcher.recursive && is_folder(real_in(watcher, key))) {
            adopt(watcher, key, &found);
        }
    } else if (!there && known != watcher.known.end()) {
        watcher.known.erase(known);
        if (watcher.recursive) {
            forget(watcher, key, found);
        }

        event.kind = EVENT_REMOVED;
        found.push_back(std::move(event));
    } else if (there) {
        found.push_back(std::move(event));
    }

    queue(watcher, std::move(found));
}

/// A folder's event without a name is libuv's word that it lost some, which a new listing finds.
/// Windows names what's in a recursive watcher's folders by their paths from it, with '\'s.
void watcher_t::on_event(uv_fs_event_t* handle, const char* name, const int events,
                         const int status) {
    auto& watcher = *static_cast<watcher_t*>(handle->data);
    if (status < 0) {
        return;
    }

    if (watcher.folder && (name == nullptr || *name == '\0')) {
        queue(watcher, rescanned(watcher));
        return;
    }

    std::string key = watcher.folder ? name : "";
#ifdef _WIN32
    std::ranges::replace(key, '\\', '/');
#endif
    told(watcher, handle->loop, key, events);
}

#ifdef __linux__
/// What happened to a name in a recursive watcher's folder's folder, as its handle tells. One
/// without a name says it lost some, as the watcher's own does.
void on_subfolder_event(uv_fs_event_t* handle, const char* name, const int events,
                        const int status) {
    auto& subfolder = *static_cast<subfolder_t*>(handle->data);
    watcher_t& watcher = *subfolder.watcher;
    if (status < 0) {
        return;
    }

    if (name == nullptr || *name == '\0') {
        queue(watcher, rescanned(watcher));
        return;
    }

    // The key's copied, since what's told may stop this handle.
    const std::string key = key_of(subfolder.key, name);
    told(watcher, handle->loop, key, events);
}
#endif

#ifdef __APPLE__
/// Lists each folder watcher's folder again, then waits for the next time, if there's one.
void rescan(uv_timer_t* timer) {
    for (watcher_t* watcher : open_watchers()) {
        if (watcher->folder) {
            queue(*watcher, rescanned(*watcher));
        }
    }

    rescans_t& state = rescans();
    if (++state.step < rescan_waits.size()) {
        uv_timer_start(timer, rescan, rescan_waits[state.step], 0);
    }
}

/// Has each folder watcher list its folder again a few times, where the OS starts watching a
/// moment after it's asked to.
void start_rescans(uv_loop_t* loop) {
    rescans_t& state = rescans();
    if (!state.made) {
        uv_timer_init(loop, &state.timer);
        state.made = true;
    }

    state.step = 0;
    uv_timer_start(&state.timer, rescan, rescan_waits[0], 0);
}
#endif

void start_watching(uv_loop_t* loop, void* context) {
    auto& watcher = *static_cast<watcher_t*>(context);
    fb::runtime::at_io_stop(stop_watchers);
    if (watcher.folder && !watcher.recursive) {
        watcher.known = names_in(watcher.real);
    } else if (!watcher.folder && exists(watcher.real)) {
        watcher.known.emplace("");
    }

    const std::int64_t made = uv_fs_event_init(loop, &watcher.handle);
    watcher.handle.data = &watcher;
    std::int64_t error = made;
    if (made == 0) {
        // Linux leaves the flag out, so each folder there is watched apart.
        const unsigned flags =
            watcher.recursive ? static_cast<unsigned>(UV_FS_EVENT_RECURSIVE) : 0U;
        error =
            uv_fs_event_start(&watcher.handle, watcher_t::on_event, watcher.path.c_str(), flags);
        if (error == 0) {
            watcher.open = true;
            open_watchers().push_back(&watcher);
            // What's in a recursive one's folders is learned once its handle watches, so what
            // comes meanwhile is told of.
            if (watcher.recursive) {
                watcher.known.emplace("");
                adopt(watcher, "", nullptr);
            }
#ifdef __linux__
            // A folder past inotify's limits would go untold of, so the watcher isn't made.
            if (watcher.unwatched != 0) {
                error = watcher.unwatched;
                close_handle(watcher);
            }
#endif
#ifdef __APPLE__
            if (watcher.folder) {
                start_rescans(loop);
            }
#endif
        } else {
            uv_close(reinterpret_cast<uv_handle_t*>(&watcher.handle), watcher_t::on_closed);
        }
    }

    {
        const std::scoped_lock lock(watcher.mutex);
        watcher.started = true;
        watcher.error = error;
    }

    watcher.changed.notify_all();
    // A handle that was never made has nothing to close, so the I/O thread lets go of it here,
    // last, since the program may let go of its own meanwhile.
    if (made != 0) {
        release(&watcher);
    }
}

/// Closes a watcher's handle, which is still open, since only this or the I/O thread's stopping
/// closes it, and that runs after the work given before it. The handle holds the watcher.
void stop_watching(uv_loop_t* /*loop*/, void* context) {
    close_handle(*static_cast<watcher_t*>(context));
}

watcher_t& watcher_of(void* watcher) {
    return *static_cast<watcher_t*>(watcher);
}

} // namespace

extern "C" {

void* fb_io_watcher_new(const fb_string_t* path, const std::int32_t recursive) {
    auto* watcher = new watcher_t;
    watcher->path = std::string(reinterpret_cast<const char*>(path + 1),
                                static_cast<std::size_t>(path->length));
    if (watcher->path.contains('\0')) {
        watcher->error = UV_EINVAL;
        return watcher;
    }

    // A folder's events name what's in it; a file's are the file's.
    uv_fs_t call{};
    if (uv_fs_stat(nullptr, &call, watcher->path.c_str(), nullptr) == 0) {
        watcher->folder = (call.statbuf.st_mode & S_IFMT) == S_IFDIR;
    }

    uv_fs_req_cleanup(&call);
    watcher->recursive = watcher->folder && recursive != 0;
    watcher->real = watcher->path;
    if (uv_fs_realpath(nullptr, &call, watcher->path.c_str(), nullptr) == 0) {
        watcher->real = static_cast<const char*>(call.ptr);
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
    fb::runtime::on_io_thread([watcher = &self](uv_loop_t* loop) {
        if (loop != nullptr) {
            stop_watching(loop, watcher);
        }
    });
}

void fb_io_watcher_free(void* watcher) {
    fb_io_watcher_close(watcher);
    release(static_cast<watcher_t*>(watcher));
}
}
