// ThreadLocal Globals: each thread's own, which a thread hands over as it makes one and lets go
// of as it ends: a pool worker's as the pool stops, a thread of its own's after its job, the main
// thread's when the program ends, and any other thread's, like one C started, as it exits.

#include "fb/runtime.hpp"
#include "internal.hpp"

#include <vector>

namespace {

/// What releases each ThreadLocal Global the thread has made, in the order it made them. What's
/// left as a thread exits, which only a thread the runtime didn't start leaves, is released then.
class thread_locals_t {
public:
    thread_locals_t() = default;
    thread_locals_t(const thread_locals_t&) = delete;
    thread_locals_t& operator=(const thread_locals_t&) = delete;
    thread_locals_t(thread_locals_t&&) = delete;
    thread_locals_t& operator=(thread_locals_t&&) = delete;

    ~thread_locals_t() {
        release();
    }

    void add(fb_release_t releaser) {
        m_releases.push_back(releaser);
    }

    /// The last made first, so its Destroy can use the ones made before it. One that a Destroy
    /// makes meanwhile is released too.
    void release() {
        while (!m_releases.empty()) {
            const fb_release_t next = m_releases.back();
            m_releases.pop_back();
            next();
        }
    }

private:
    std::vector<fb_release_t> m_releases;
};

thread_local thread_locals_t t_locals;
/// Whether fb_thread_locals_release has run on this thread.
thread_local bool t_released = false;

} // namespace

void fb::runtime::release_thread_locals() {
    t_locals.release();
}

bool fb::runtime::released_thread_locals() {
    return t_released;
}

extern "C" {

void fb_thread_local_added(const fb_release_t release) {
    fb::runtime::ready_letting_go();
    t_locals.add(release);
}

void fb_thread_locals_release() {
    t_released = true;
    t_locals.release();
}

} // extern "C"
