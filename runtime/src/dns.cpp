// std.net's Dns: the addresses a host's name has, from libuv's getaddrinfo on its thread pool,
// which the I/O thread starts, for a caller that waits or a Task it finishes. Each address goes
// in the output array std.net gives as 17 bytes: 4 or 6 for its family, then its 16 bytes, an
// IPv4 address's 4 first.

#include "fb/runtime.hpp"
#include "internal.hpp"
#include "io_internal.hpp"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <uv.h>
#include <vector>

namespace {

constexpr std::size_t record_size = 17;

/// What waits for a lookup: a blocking caller, woken with its result, or a Task<Int32> to
/// finish with it. Either way, `output` is the caller's UInt8[], which it holds until then.
struct lookup_t {
    uv_getaddrinfo_t request{};
    std::string host;
    fb_array_t* output = nullptr;
    fb_task_t* task = nullptr;
    std::mutex mutex;
    std::condition_variable answered;
    bool done = false;
    std::int32_t result = 0;

    /// libuv's callback, once getaddrinfo has the addresses or failed.
    static void on_resolved(uv_getaddrinfo_t* request, int status, addrinfo* found);
};

/// Puts each address `found` has in `output`, once, as getaddrinfo gives one for each kind of
/// socket it could have.
void put_addresses(const addrinfo* found, fb_array_t* output) {
    std::vector<std::uint8_t> records;
    for (const addrinfo* entry = found; entry != nullptr; entry = entry->ai_next) {
        std::array<std::uint8_t, record_size> record{};
        if (entry->ai_family == AF_INET) {
            record[0] = 4;
            const auto* address = reinterpret_cast<const sockaddr_in*>(entry->ai_addr);
            std::memcpy(&record[1], &address->sin_addr, 4);
        } else if (entry->ai_family == AF_INET6) {
            record[0] = 6;
            const auto* address = reinterpret_cast<const sockaddr_in6*>(entry->ai_addr);
            std::memcpy(&record[1], &address->sin6_addr, 16);
        } else {
            continue;
        }

        bool seen = false;
        for (std::size_t at = 0; at < records.size() && !seen; at += record_size) {
            seen = std::equal(record.begin(), record.end(),
                              records.begin() + static_cast<std::ptrdiff_t>(at));
        }

        if (!seen) {
            records.insert(records.end(), record.begin(), record.end());
        }
    }

    output->count = 0;
    if (!records.empty()) {
        fb_array_reserve(output, 1, static_cast<std::int64_t>(records.size()), nullptr);
        std::memcpy(output->data, records.data(), records.size());
        output->count = static_cast<std::int64_t>(records.size());
    }
}

/// Gives the lookup its result: its caller wakes, or its Task finishes.
void finish(lookup_t* lookup, const std::int32_t result) {
    fb_task_t* task = lookup->task;
    // The caller can go, and its lookup with it, once it has the lock again, so it's told under
    // the lock.
    if (task == nullptr) {
        const std::scoped_lock lock(lookup->mutex);
        lookup->result = result;
        lookup->done = true;
        lookup->answered.notify_all();
        return;
    }

    // The Task's result, an Int32, is right after its fb_task_t, and it's done once that's set.
    *reinterpret_cast<std::int32_t*>(task + 1) = result;
    delete lookup;
    fb_task_finish(task);
    fb::runtime::finish_request(task);
    fb_object_release(&task->header);
}

void lookup_t::on_resolved(uv_getaddrinfo_t* request, const int status, addrinfo* found) {
    auto* lookup = static_cast<lookup_t*>(request->data);
    if (status == 0) {
        put_addresses(found, lookup->output);
    }

    uv_freeaddrinfo(found);
    finish(lookup, status);
}

/// Starts the lookup on the I/O thread, or finishes it with UV_ECANCELED once that's stopping.
void start(lookup_t* lookup) {
    fb::runtime::on_io_thread([lookup](uv_loop_t* loop) {
        if (loop == nullptr) {
            finish(lookup, UV_ECANCELED);
            return;
        }

        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        lookup->request.data = lookup;
        const int started = uv_getaddrinfo(loop, &lookup->request, lookup_t::on_resolved,
                                           lookup->host.c_str(), nullptr, &hints);
        if (started != 0) {
            finish(lookup, started);
        }
    });
}

std::string text_of(const fb_string_t* text) {
    return { reinterpret_cast<const char*>(text + 1), static_cast<std::size_t>(text->length) };
}

} // namespace

extern "C" {

std::int32_t fb_io_resolve(const fb_string_t* host, fb_array_t* output) {
    lookup_t lookup;
    lookup.host = text_of(host);
    lookup.output = output;
    start(&lookup);
    std::unique_lock lock(lookup.mutex);
    lookup.answered.wait(lock, [&] { return lookup.done; });
    return lookup.result;
}

void fb_io_resolve_async(fb_task_t* task, const fb_string_t* host, fb_array_t* output) {
    // The lookup holds the Task until it's done.
    auto* lookup = new lookup_t;
    lookup->host = text_of(host);
    lookup->output = output;
    lookup->task = task;
    fb_object_retain(&task->header);
    fb::runtime::start_request(task);
    start(lookup);
}

fb_string_t* fb_io_error_text(const std::int32_t code) {
    return fb_string_from_cstring(uv_strerror(code));
}
}
