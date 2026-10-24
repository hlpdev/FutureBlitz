// std.net.http's client over libcurl, whose TLS is mbedTLS's, trusting the OS's roots unless a
// client was given its own. A blocking transfer runs on its caller's thread, through its client's
// multi handle, which keeps the client's connections; an Async one runs on the I/O thread,
// through a multi handle whose sockets and timer are libuv's there, and finishes its Task as it
// ends. A client's share handle keeps its cookies, DNS answers, and TLS sessions for both.

#include "fb/http.hpp"

#include "fb/runtime.hpp"
#include "internal.hpp"
#include "io_internal.hpp"
#include "tls_roots.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <curl/curl.h>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <uv.h>
#include <vector>

#ifndef _WIN32
#include <cerrno>
#endif

namespace {

/// A transfer's outcome, as fb/http.hpp numbers them.
enum outcome_t : std::int8_t {
    OUTCOME_RESPONSE = 0,
    OUTCOME_HOST_NOT_FOUND = 1,
    OUTCOME_REFUSED = 2,
    OUTCOME_UNREACHABLE = 3,
    OUTCOME_RESET = 4,
    OUTCOME_CLOSED = 5,
    OUTCOME_TLS = 6,
    OUTCOME_TIMEOUT = 7,
    OUTCOME_OTHER = 8,
    OUTCOME_BAD_URL = 9,
    OUTCOME_REDIRECTS = 10,
    OUTCOME_ENDED = 11,
};

/// Sets up libcurl, once, before its first handle.
void start_curl() {
    static const CURLcode started = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (started != CURLE_OK) {
        fb_panic("std.net.http can't start libcurl", nullptr);
    }
}

std::string_view view_of(const fb_string_t* text) {
    return { reinterpret_cast<const char*>(text + 1), static_cast<std::size_t>(text->length) };
}

std::span<const std::uint8_t> bytes_of(const fb_array_t* array) {
    return { static_cast<const std::uint8_t*>(array->data),
             static_cast<std::size_t>(array->count) };
}

fb_string_t* string_of(const std::string_view text) {
    return fb_string_from_bytes(text.data(), static_cast<std::int64_t>(text.size()));
}

/// Panics when libcurl refuses what std.net.http checked, as when memory runs out.
void check(const CURLcode code, const char* what) {
    if (code != CURLE_OK) {
        std::array<char, 160> message{};
        std::snprintf(message.data(), message.size(), "std.net.http's %s failed: %s", what,
                      curl_easy_strerror(code));
        fb_panic(message.data(), nullptr);
    }
}

/// What a client keeps for its transfers. Its transfers hold it too, so it lasts as long as
/// any of them.
struct client_t {
    std::atomic<std::int64_t> owners = 1;
    /// Its cookies, DNS answers, and TLS sessions, which any thread's transfers share.
    CURLSH* share = nullptr;
    /// The connections of its blocking transfers, on the client's own thread.
    CURLM* multi = nullptr;
    std::array<std::mutex, CURL_LOCK_DATA_LAST> locks;
    /// The certificates it trusts, as PEM, when it was given its own.
    std::optional<std::string> roots;
};

void retain(client_t* client) {
    client->owners.fetch_add(1, std::memory_order_relaxed);
}

void release(client_t* client) {
    if (client->owners.fetch_sub(1, std::memory_order_acq_rel) != 1) {
        return;
    }

    if (client->multi != nullptr) {
        curl_multi_cleanup(client->multi);
    }

    curl_share_cleanup(client->share);
    delete client;
}

void lock_share(CURL* /*easy*/, const curl_lock_data data, curl_lock_access /*access*/,
                void* client) {
    static_cast<client_t*>(client)->locks.at(data).lock();
}

void unlock_share(CURL* /*easy*/, const curl_lock_data data, void* client) {
    static_cast<client_t*>(client)->locks.at(data).unlock();
}

/// One request and its response. std.net.http holds it, and so does the I/O thread while it
/// runs there.
struct transfer_t {
    std::atomic<std::int64_t> owners = 1;
    client_t* client = nullptr;
    CURL* easy = nullptr;
    /// The request as it's sent now, which a redirect can change: its method, its headers as
    /// they were given, and its body, if it has one, and libcurl's list of the headers.
    std::string method;
    std::vector<std::pair<std::string, std::string>> fields;
    std::optional<std::vector<std::uint8_t>> body;
    curl_slist* headers = nullptr;
    /// The values of its own Cookie headers, which libcurl sends with those it keeps.
    std::string cookies;
    /// The most redirects it follows, or -1 to follow none, and how many it followed.
    std::int32_t redirects = -1;
    std::int32_t followed = 0;
    std::string user_agent;
    std::string proxy;
    /// The response's body, as it comes.
    std::vector<std::uint8_t> received;
    std::array<char, CURL_ERROR_SIZE> error{};
    /// Whether its options are all set, and it's in its client's multi handle, for a blocking
    /// one.
    bool ready = false;
    bool added = false;
    /// What the thread it runs on and the one that waits for it share.
    std::mutex mutex;
    std::int64_t downloaded = 0;
    std::int64_t expected = -1;
    /// How much had come when it last said so.
    std::int64_t reported = 0;
    bool started = false;
    bool done = false;
    /// It's let go of while it runs on the I/O thread, which stops it.
    bool abandoned = false;
    std::int32_t outcome = OUTCOME_RESPONSE;
    /// The Task that waits for its end, or for more of it when `progress` is set.
    fb_task_t* waiting = nullptr;
    bool progress = false;
};

void release(transfer_t* transfer) {
    if (transfer->owners.fetch_sub(1, std::memory_order_acq_rel) != 1) {
        return;
    }

    curl_easy_cleanup(transfer->easy);
    curl_slist_free_all(transfer->headers);
    release(transfer->client);
    delete transfer;
}

transfer_t* transfer_of(CURL* easy) {
    char* transfer = nullptr;
    curl_easy_getinfo(easy, CURLINFO_PRIVATE, &transfer);
    return reinterpret_cast<transfer_t*>(transfer);
}

std::size_t on_data(char* data, const std::size_t size, const std::size_t count, void* transfer) {
    const std::size_t length = size * count;
    auto& received = static_cast<transfer_t*>(transfer)->received;
    received.insert(received.end(), data, data + length);
    return length;
}

/// Finishes `task`, a Task<Int32>, with `value`, letting go of it.
void answer(fb_task_t* task, const std::int32_t value, const bool requested) {
    *reinterpret_cast<std::int32_t*>(task + 1) = value;
    fb_task_finish(task);
    if (requested) {
        fb::runtime::finish_request(task);
    }

    fb_object_release(&task->header);
}

int on_progress(void* data, const curl_off_t expected, const curl_off_t downloaded,
                curl_off_t /*to_send*/, curl_off_t /*sent*/) {
    auto& transfer = *static_cast<transfer_t*>(data);
    fb_task_t* waiting = nullptr;
    bool abandoned = false;
    {
        const std::scoped_lock lock(transfer.mutex);
        transfer.downloaded = downloaded;
        transfer.expected = expected > 0 ? expected : -1;
        if (transfer.waiting != nullptr && transfer.progress &&
            transfer.downloaded != transfer.reported) {
            transfer.reported = transfer.downloaded;
            waiting = std::exchange(transfer.waiting, nullptr);
        }

        abandoned = transfer.abandoned;
    }

    if (waiting != nullptr) {
        answer(waiting, 0, true);
    }

    // Anything but 0 stops it.
    return abandoned ? 1 : 0;
}

/// The OS's error for the connection that failed, as libcurl gives it.
outcome_t connect_outcome(CURL* easy) {
    long error = 0;
    curl_easy_getinfo(easy, CURLINFO_OS_ERRNO, &error);
    switch (error) {
#ifdef _WIN32
    case WSAENETUNREACH:
    case WSAEHOSTUNREACH:
    case WSAENETDOWN:
#else
    case ENETUNREACH:
    case EHOSTUNREACH:
    case ENETDOWN:
#endif
        return OUTCOME_UNREACHABLE;
#ifdef _WIN32
    case WSAETIMEDOUT:
#else
    case ETIMEDOUT:
#endif
        return OUTCOME_TIMEOUT;
    default:
        return OUTCOME_REFUSED;
    }
}

/// What a transfer's result says failed, or that it got a response.
outcome_t outcome_of(CURL* easy, const CURLcode code) {
    switch (code) {
    case CURLE_OK:
        return OUTCOME_RESPONSE;
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_RESOLVE_PROXY:
        return OUTCOME_HOST_NOT_FOUND;
    case CURLE_COULDNT_CONNECT:
        return connect_outcome(easy);
    case CURLE_OPERATION_TIMEDOUT:
        return OUTCOME_TIMEOUT;
    case CURLE_RECV_ERROR:
        return OUTCOME_RESET;
    // libcurl gives a send error, too, when each connection it tried closed before any response.
    case CURLE_SEND_ERROR:
    case CURLE_GOT_NOTHING:
    case CURLE_PARTIAL_FILE:
        return OUTCOME_CLOSED;
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CERTPROBLEM:
    case CURLE_SSL_CIPHER:
    case CURLE_SSL_CACERT_BADFILE:
    case CURLE_SSL_ISSUER_ERROR:
    case CURLE_SSL_INVALIDCERTSTATUS:
    case CURLE_SSL_SHUTDOWN_FAILED:
    case CURLE_SSL_CLIENTCERT:
    case CURLE_USE_SSL_FAILED:
        return OUTCOME_TLS;
    case CURLE_URL_MALFORMAT:
    case CURLE_UNSUPPORTED_PROTOCOL:
        return OUTCOME_BAD_URL;
    case CURLE_TOO_MANY_REDIRECTS:
        return OUTCOME_REDIRECTS;
    case CURLE_ABORTED_BY_CALLBACK:
        return OUTCOME_ENDED;
    default:
        return OUTCOME_OTHER;
    }
}

bool same_name(const std::string_view a, const std::string_view b) {
    return a.size() == b.size() && std::ranges::equal(a, b, [](const char x, const char y) {
               const auto lower = [](const char c) {
                   return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
               };
               return lower(x) == lower(y);
           });
}

/// Sets the request's method, body, and headers on its handle, for its first request or for a
/// redirect's. The headers keep libcurl's own Content-Type for a body and Expect out unless they
/// were given.
void set_request(transfer_t& transfer) {
    CURL* easy = transfer.easy;
    // A GET first, which takes another method's settings off.
    curl_easy_setopt(easy, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, nullptr);
    const std::string& method = transfer.method;
    if (method == "HEAD") {
        curl_easy_setopt(easy, CURLOPT_NOBODY, 1L);
    } else if (transfer.body || method == "POST") {
        // A POST without a body sends an empty one.
        static const char nothing = 0;
        const std::vector<std::uint8_t>* body = transfer.body ? &*transfer.body : nullptr;
        curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE_LARGE,
                         static_cast<curl_off_t>(body != nullptr ? body->size() : 0));
        curl_easy_setopt(easy, CURLOPT_POSTFIELDS,
                         body != nullptr && !body->empty() ? static_cast<const void*>(body->data())
                                                           : static_cast<const void*>(&nothing));
    }

    if (method != "HEAD" && method != "POST" && (method != "GET" || transfer.body)) {
        curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, method.c_str());
    }

    curl_slist* headers = nullptr;
    bool content_type = false;
    bool expect = false;
    const auto add = [&](const std::string& line) {
        headers = curl_slist_append(headers, line.c_str());
        if (headers == nullptr) {
            fb_panic("out of memory", nullptr);
        }
    };
    // The request's own cookies go with the ones libcurl keeps, in its one Cookie header.
    transfer.cookies.clear();
    for (const auto& [name, value] : transfer.fields) {
        if (same_name(name, "Cookie")) {
            if (!transfer.cookies.empty()) {
                transfer.cookies += "; ";
            }

            transfer.cookies += value;
            continue;
        }

        content_type = content_type || same_name(name, "Content-Type");
        expect = expect || same_name(name, "Expect");
        // libcurl takes "Name:" as taking its own header out, and "Name;" as one with no value.
        std::string line = name;
        if (value.empty()) {
            line += ';';
        } else {
            line += ": ";
            line += value;
        }

        add(line);
    }

    curl_easy_setopt(easy, CURLOPT_COOKIE,
                     transfer.cookies.empty() ? nullptr : transfer.cookies.c_str());

    if (!content_type) {
        add("Content-Type:");
    }

    if (!expect) {
        add("Expect:");
    }

    check(curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers), "request");
    curl_slist_free_all(std::exchange(transfer.headers, headers));
}

/// Sets the request up once it's all given.
void make_ready(transfer_t& transfer) {
    if (!transfer.ready) {
        transfer.ready = true;
        set_request(transfer);
    }
}

/// A URL's scheme, host, and port, which two URLs of one origin share.
std::string origin_of(const char* url) {
    std::string origin;
    CURLU* parsed = curl_url();
    if (parsed != nullptr && curl_url_set(parsed, CURLUPART_URL, url, 0) == CURLUE_OK) {
        for (const CURLUPart part : { CURLUPART_SCHEME, CURLUPART_HOST, CURLUPART_PORT }) {
            char* text = nullptr;
            if (curl_url_get(parsed, part, &text, CURLU_DEFAULT_PORT) == CURLUE_OK) {
                for (const char* c = text; *c != '\0'; ++c) {
                    origin += *c >= 'A' && *c <= 'Z' ? static_cast<char>(*c - 'A' + 'a') : *c;
                }

                curl_free(text);
            }

            origin += ' ';
        }
    }

    curl_url_cleanup(parsed);
    return origin;
}

/// What comes after a response: the transfer's end, the redirect it goes on to, or its end
/// because there were too many.
enum class next_t : std::uint8_t { END, REDIRECT, TOO_MANY };

/// After a response, sends the transfer on where a redirect says, as the fetch standard has it:
/// a 301 or a 302 changes a POST to a GET, and a 303 any method but HEAD, without the body and
/// the headers that say what it is, and a redirect to another origin sends no Authorization,
/// Cookie, or Host of the request's own. libcurl sends the cookies it keeps for each URL. Its
/// handle is out of any multi handle.
next_t redirect(transfer_t& transfer) {
    long status = 0;
    curl_easy_getinfo(transfer.easy, CURLINFO_RESPONSE_CODE, &status);
    const char* location = nullptr;
    curl_easy_getinfo(transfer.easy, CURLINFO_REDIRECT_URL, &location);
    if (transfer.redirects < 0 || location == nullptr ||
        (status != 301 && status != 302 && status != 303 && status != 307 && status != 308)) {
        return next_t::END;
    }

    if (transfer.followed == transfer.redirects) {
        std::snprintf(transfer.error.data(), transfer.error.size(),
                      "it was sent on more than the %d redirects it follows",
                      static_cast<int>(transfer.redirects));
        return next_t::TOO_MANY;
    }

    const std::string target = location;
    const char* current = nullptr;
    curl_easy_getinfo(transfer.easy, CURLINFO_EFFECTIVE_URL, &current);
    const auto drop = [&](const std::initializer_list<std::string_view> names) {
        std::erase_if(transfer.fields, [&](const auto& field) {
            return std::ranges::any_of(
                names, [&](const auto name) { return same_name(field.first, name); });
        });
    };
    const std::string& method = transfer.method;
    if (((status == 301 || status == 302) && method == "POST") ||
        (status == 303 && method != "GET" && method != "HEAD")) {
        transfer.method = "GET";
        transfer.body.reset();
        drop({ "Content-Type", "Content-Encoding", "Content-Language", "Content-Location" });
    }

    if (origin_of(current) != origin_of(target.c_str())) {
        drop({ "Authorization", "Cookie", "Host" });
    }

    ++transfer.followed;
    transfer.received.clear();
    {
        const std::scoped_lock lock(transfer.mutex);
        transfer.downloaded = 0;
        transfer.reported = 0;
        transfer.expected = -1;
    }

    check(curl_easy_setopt(transfer.easy, CURLOPT_URL, target.c_str()), "redirect");
    set_request(transfer);
    return next_t::REDIRECT;
}

/// The outcome of a transfer whose handle ended with `result`, after any redirect it went on to,
/// which `go_on` starts: nothing while it goes on.
template <typename GoOn>
std::optional<std::int32_t> outcome_after(transfer_t& transfer, const CURLcode result,
                                          const GoOn& go_on) {
    if (result != CURLE_OK) {
        return outcome_of(transfer.easy, result);
    }

    switch (redirect(transfer)) {
    case next_t::REDIRECT:
        if (go_on()) {
            return std::nullopt;
        }

        return OUTCOME_OTHER;
    case next_t::TOO_MANY:
        return OUTCOME_REDIRECTS;
    case next_t::END:
        break;
    }

    return OUTCOME_RESPONSE;
}

/// Ends each of a client's blocking transfers that's done, with its outcome.
void settle_blocking(CURLM* multi) {
    int left = 0;
    for (const CURLMsg* message = curl_multi_info_read(multi, &left); message != nullptr;
         message = curl_multi_info_read(multi, &left)) {
        if (message->msg != CURLMSG_DONE) {
            continue;
        }

        CURL* easy = message->easy_handle;
        const CURLcode result = message->data.result;
        transfer_t& done = *transfer_of(easy);
        curl_multi_remove_handle(multi, easy);
        done.added = false;
        const std::optional<std::int32_t> outcome = outcome_after(done, result, [&] {
            done.added = curl_multi_add_handle(multi, easy) == CURLM_OK;
            return done.added;
        });
        if (outcome) {
            const std::scoped_lock lock(done.mutex);
            done.outcome = *outcome;
            done.done = true;
        }
    }
}

/// Runs a blocking transfer through its client's multi handle until it's done, or, when
/// `until_more`, until more of it came. Gives its outcome plus 1, or 0 for more.
std::int32_t run_blocking(transfer_t& transfer, const bool until_more) {
    make_ready(transfer);
    client_t& client = *transfer.client;
    if (client.multi == nullptr) {
        client.multi = curl_multi_init();
        if (client.multi == nullptr) {
            fb_panic("out of memory", nullptr);
        }
    }

    if (!transfer.started) {
        transfer.started = true;
        if (curl_multi_add_handle(client.multi, transfer.easy) != CURLM_OK) {
            fb_panic("std.net.http can't start a transfer", nullptr);
        }

        transfer.added = true;
    }

    while (true) {
        {
            const std::scoped_lock lock(transfer.mutex);
            if (transfer.done) {
                return transfer.outcome + 1;
            }

            if (until_more && transfer.downloaded != transfer.reported) {
                transfer.reported = transfer.downloaded;
                return 0;
            }
        }

        int running = 0;
        curl_multi_perform(client.multi, &running);
        settle_blocking(client.multi);
        {
            const std::scoped_lock lock(transfer.mutex);
            if (transfer.done || (until_more && transfer.downloaded != transfer.reported)) {
                continue;
            }
        }

        curl_multi_poll(client.multi, nullptr, 0, 1000, nullptr);
    }
}

// The I/O thread's part: its multi handle, whose sockets libuv polls and whose timer is
// libuv's, and the transfers running there. Only the I/O thread touches it.

struct io_part_t;

/// A socket libcurl has the loop watch.
struct poll_t {
    uv_poll_t handle{};
    curl_socket_t socket{};
    io_part_t* part = nullptr;
};

struct io_part_t {
    uv_loop_t* loop = nullptr;
    CURLM* multi = nullptr;
    uv_timer_t timer{};
    std::vector<poll_t*> polls;
    std::vector<transfer_t*> running;
};

io_part_t*& io_part() {
    static io_part_t* part = nullptr;
    return part;
}

/// A transfer that ran on the I/O thread is done, there or because the program is ending: what
/// waits for it goes on, and the thread lets go of it.
void end(transfer_t* transfer, const std::int32_t outcome) {
    fb_task_t* waiting = nullptr;
    {
        const std::scoped_lock lock(transfer->mutex);
        transfer->outcome = outcome;
        transfer->done = true;
        waiting = std::exchange(transfer->waiting, nullptr);
    }

    if (waiting != nullptr) {
        answer(waiting, outcome + 1, true);
    }

    release(transfer);
}

/// Ends each of the I/O thread's transfers that's done.
void settle(io_part_t& part) {
    int left = 0;
    for (const CURLMsg* message = curl_multi_info_read(part.multi, &left); message != nullptr;
         message = curl_multi_info_read(part.multi, &left)) {
        if (message->msg != CURLMSG_DONE) {
            continue;
        }

        CURL* easy = message->easy_handle;
        const CURLcode result = message->data.result;
        transfer_t* transfer = transfer_of(easy);
        curl_multi_remove_handle(part.multi, easy);
        const std::optional<std::int32_t> outcome = outcome_after(
            *transfer, result, [&] { return curl_multi_add_handle(part.multi, easy) == CURLM_OK; });
        if (outcome) {
            std::erase(part.running, transfer);
            end(transfer, *outcome);
        }
    }
}

void close_poll(poll_t* poll) {
    std::erase(poll->part->polls, poll);
    uv_poll_stop(&poll->handle);
    uv_close(reinterpret_cast<uv_handle_t*>(&poll->handle),
             [](uv_handle_t* handle) { delete static_cast<poll_t*>(handle->data); });
}

/// `flag` if `events` has `event`, or none.
unsigned flag_of(const int events, const int event, const int flag) {
    return (static_cast<unsigned>(events) & static_cast<unsigned>(event)) != 0
               ? static_cast<unsigned>(flag)
               : 0U;
}

void on_poll(uv_poll_t* handle, const int status, const int events) {
    // The handle is its poll_t's first member.
    const auto& poll = *reinterpret_cast<poll_t*>(handle);
    io_part_t& part = *poll.part;
    int flags = CURL_CSELECT_ERR;
    if (status >= 0) {
        flags = static_cast<int>(flag_of(events, UV_READABLE, CURL_CSELECT_IN) |
                                 flag_of(events, UV_WRITABLE, CURL_CSELECT_OUT));
    }

    int running = 0;
    curl_multi_socket_action(part.multi, poll.socket, flags, &running);
    settle(part);
}

/// libcurl says which of a socket's events the loop watches for, or that it's done with it.
int on_socket(CURL* /*easy*/, const curl_socket_t socket, const int what, void* data,
              void* socket_data) {
    auto& part = *static_cast<io_part_t*>(data);
    auto* poll = static_cast<poll_t*>(socket_data);
    if (what == CURL_POLL_REMOVE) {
        if (poll != nullptr) {
            curl_multi_assign(part.multi, socket, nullptr);
            close_poll(poll);
        }

        return 0;
    }

    if (poll == nullptr) {
        poll = new poll_t{ .socket = socket, .part = &part };
        if (uv_poll_init_socket(part.loop, &poll->handle, socket) != 0) {
            delete poll;
            return -1;
        }

        poll->handle.data = poll;
        part.polls.push_back(poll);
        curl_multi_assign(part.multi, socket, static_cast<void*>(poll));
    }

    const auto events = static_cast<int>(flag_of(what, CURL_POLL_IN, UV_READABLE) |
                                         flag_of(what, CURL_POLL_OUT, UV_WRITABLE));
    uv_poll_start(&poll->handle, events, on_poll);
    return 0;
}

void on_timeout(uv_timer_t* /*timer*/) {
    io_part_t& part = *io_part();
    int running = 0;
    curl_multi_socket_action(part.multi, CURL_SOCKET_TIMEOUT, 0, &running);
    settle(part);
}

/// libcurl says when it next wants to run, or -1 for no time.
int on_timer(CURLM* /*multi*/, const long milliseconds, void* data) {
    auto& part = *static_cast<io_part_t*>(data);
    if (milliseconds < 0) {
        uv_timer_stop(&part.timer);
    } else {
        uv_timer_start(&part.timer, on_timeout, static_cast<std::uint64_t>(milliseconds), 0);
    }

    return 0;
}

/// The I/O thread's part, which it sets up the first time.
io_part_t& part_on(uv_loop_t* loop) {
    io_part_t*& part = io_part();
    if (part == nullptr) {
        part = new io_part_t{ .loop = loop, .multi = curl_multi_init() };
        if (part->multi == nullptr) {
            fb_panic("out of memory", nullptr);
        }

        uv_timer_init(loop, &part->timer);
        part->timer.data = part;
        curl_multi_setopt(part->multi, CURLMOPT_SOCKETFUNCTION, on_socket);
        curl_multi_setopt(part->multi, CURLMOPT_SOCKETDATA, static_cast<void*>(part));
        curl_multi_setopt(part->multi, CURLMOPT_TIMERFUNCTION, on_timer);
        curl_multi_setopt(part->multi, CURLMOPT_TIMERDATA, static_cast<void*>(part));
    }

    return *part;
}

/// Starts a transfer on the I/O thread, or ends it at once as the program ends.
void start_on_io(uv_loop_t* loop, transfer_t* transfer) {
    if (loop == nullptr) {
        end(transfer, OUTCOME_ENDED);
        return;
    }

    io_part_t& part = part_on(loop);
    if (curl_multi_add_handle(part.multi, transfer->easy) != CURLM_OK) {
        end(transfer, OUTCOME_OTHER);
        return;
    }

    part.running.push_back(transfer);
}

/// As the program ends: ends what still runs, and lets go of the part, its sockets and its timer
/// last.
void stop_on_io(uv_loop_t* /*loop*/) {
    io_part_t* part = std::exchange(io_part(), nullptr);
    if (part == nullptr) {
        return;
    }

    for (transfer_t* transfer : std::exchange(part->running, {})) {
        curl_multi_remove_handle(part->multi, transfer->easy);
        end(transfer, OUTCOME_ENDED);
    }

    for (poll_t* poll : std::vector(part->polls)) {
        close_poll(poll);
    }

    curl_multi_setopt(part->multi, CURLMOPT_SOCKETFUNCTION, nullptr);
    curl_multi_setopt(part->multi, CURLMOPT_TIMERFUNCTION, nullptr);
    curl_multi_cleanup(part->multi);
    uv_timer_stop(&part->timer);
    uv_close(reinterpret_cast<uv_handle_t*>(&part->timer),
             [](uv_handle_t* handle) { delete static_cast<io_part_t*>(handle->data); });
}

} // namespace

extern "C" {

void* fb_http_client_new() {
    start_curl();
    auto* client = new client_t();
    client->share = curl_share_init();
    if (client->share == nullptr) {
        fb_panic("out of memory", nullptr);
    }

    curl_share_setopt(client->share, CURLSHOPT_LOCKFUNC, lock_share);
    curl_share_setopt(client->share, CURLSHOPT_UNLOCKFUNC, unlock_share);
    curl_share_setopt(client->share, CURLSHOPT_USERDATA, static_cast<void*>(client));
    curl_share_setopt(client->share, CURLSHOPT_SHARE, CURL_LOCK_DATA_COOKIE);
    curl_share_setopt(client->share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
    curl_share_setopt(client->share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
    return client;
}

void fb_http_client_free(void* client) {
    release(static_cast<client_t*>(client));
}

void fb_http_client_clear_cookies(void* client) {
    // Only a transfer's handle reaches the share's cookies.
    CURL* easy = curl_easy_init();
    if (easy == nullptr) {
        fb_panic("out of memory", nullptr);
    }

    curl_easy_setopt(easy, CURLOPT_SHARE, static_cast<client_t*>(client)->share);
    curl_easy_setopt(easy, CURLOPT_COOKIELIST, "ALL");
    curl_easy_cleanup(easy);
}

void fb_http_client_trust_own(void* client, const std::int32_t own) {
    auto& roots = static_cast<client_t*>(client)->roots;
    if (own != 0) {
        roots.emplace();
    } else {
        roots.reset();
    }
}

void fb_http_client_trust(void* client, const fb_array_t* der) {
    auto& roots = static_cast<client_t*>(client)->roots;
    if (!roots) {
        roots.emplace();
    }

    fb::runtime::append_pem(*roots, bytes_of(der));
}

void* fb_http_new(void* client, const fb_string_t* method, const fb_string_t* url) {
    auto* transfer = new transfer_t();
    transfer->client = static_cast<client_t*>(client);
    retain(transfer->client);
    transfer->easy = curl_easy_init();
    if (transfer->easy == nullptr) {
        fb_panic("out of memory", nullptr);
    }

    CURL* easy = transfer->easy;
    const std::string address(view_of(url));
    transfer->method = view_of(method);
    check(curl_easy_setopt(easy, CURLOPT_URL, address.c_str()), "URL");
    curl_easy_setopt(easy, CURLOPT_PRIVATE, static_cast<void*>(transfer));
    curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, transfer->error.data());
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    // It follows redirects itself, in redirect(), to http and https URLs only. It speaks HTTP/2
    // where TLS's ALPN agrees on it, and HTTP/1.1 otherwise.
    curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
    curl_easy_setopt(easy, CURLOPT_SHARE, transfer->client->share);
    // An empty name turns on the cookie engine, with no file to read.
    curl_easy_setopt(easy, CURLOPT_COOKIEFILE, "");
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, static_cast<void*>(transfer));
    curl_easy_setopt(easy, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(easy, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(easy, CURLOPT_XFERINFODATA, static_cast<void*>(transfer));
    // The client's own roots, which libcurl copies, or the OS's, which last as long as the
    // program. An empty bundle is none, so nothing's trusted.
    const std::optional<std::string>& own = transfer->client->roots;
    const std::string& roots = own ? *own : fb::runtime::os_roots();
    if (!roots.empty()) {
        curl_blob blob{
            .data = const_cast<char*>(roots.data()),
            .len = roots.size(),
            .flags = own ? static_cast<unsigned>(CURL_BLOB_COPY)
                         : static_cast<unsigned>(CURL_BLOB_NOCOPY),
        };
        check(curl_easy_setopt(easy, CURLOPT_CAINFO_BLOB, &blob), "roots");
    }

    return transfer;
}

void fb_http_header(void* transfer, const fb_string_t* name, const fb_string_t* value) {
    static_cast<transfer_t*>(transfer)->fields.emplace_back(view_of(name), view_of(value));
}

void fb_http_body(void* transfer, const fb_array_t* body) {
    const std::span<const std::uint8_t> bytes = bytes_of(body);
    static_cast<transfer_t*>(transfer)->body.emplace(bytes.begin(), bytes.end());
}

void fb_http_options(void* transfer, const std::int64_t timeout, const std::int64_t connect_timeout,
                     const std::int32_t redirects, const std::int32_t decompress) {
    auto& given = *static_cast<transfer_t*>(transfer);
    CURL* easy = given.easy;
    curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout));
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(connect_timeout));
    given.redirects = redirects;

    // An empty list asks for every encoding libcurl decodes.
    curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, decompress != 0 ? "" : nullptr);
}

void fb_http_proxy(void* transfer, const fb_string_t* proxy) {
    auto& given = *static_cast<transfer_t*>(transfer);
    given.proxy = view_of(proxy);
    check(curl_easy_setopt(given.easy, CURLOPT_PROXY, given.proxy.c_str()), "proxy");
}

void fb_http_user_agent(void* transfer, const fb_string_t* agent) {
    auto& given = *static_cast<transfer_t*>(transfer);
    given.user_agent = view_of(agent);
    check(curl_easy_setopt(given.easy, CURLOPT_USERAGENT, given.user_agent.c_str()), "User-Agent");
}

std::int32_t fb_http_perform(void* transfer) {
    return run_blocking(*static_cast<transfer_t*>(transfer), false) - 1;
}

std::int32_t fb_http_advance(void* transfer) {
    return run_blocking(*static_cast<transfer_t*>(transfer), true);
}

void fb_http_wait_async(fb_task_t* task, void* transfer, const std::int32_t progress) {
    auto& given = *static_cast<transfer_t*>(transfer);
    // The request holds the Task until it's done.
    fb_object_retain(&task->header);
    bool start = false;
    {
        std::unique_lock lock(given.mutex);
        if (given.done) {
            const std::int32_t outcome = given.outcome;
            lock.unlock();
            answer(task, outcome + 1, false);
            return;
        }

        if (progress != 0 && given.started && given.downloaded != given.reported) {
            given.reported = given.downloaded;
            lock.unlock();
            answer(task, 0, false);
            return;
        }

        given.waiting = task;
        given.progress = progress != 0;
        fb::runtime::start_request(task);
        start = !given.started;
        given.started = true;
    }

    if (start) {
        static std::once_flag registered;
        std::call_once(registered, [] { fb::runtime::at_io_stop(stop_on_io); });
        make_ready(given);
        // The I/O thread holds the transfer while it runs there.
        given.owners.fetch_add(1, std::memory_order_relaxed);
        fb::runtime::on_io_thread([&given](uv_loop_t* loop) { start_on_io(loop, &given); });
    }
}

std::int64_t fb_http_received(void* transfer) {
    auto& given = *static_cast<transfer_t*>(transfer);
    const std::scoped_lock lock(given.mutex);
    return given.downloaded;
}

std::int64_t fb_http_expected(void* transfer) {
    auto& given = *static_cast<transfer_t*>(transfer);
    const std::scoped_lock lock(given.mutex);
    return given.expected;
}

std::int32_t fb_http_status(void* transfer) {
    long status = 0;
    curl_easy_getinfo(static_cast<transfer_t*>(transfer)->easy, CURLINFO_RESPONSE_CODE, &status);
    return static_cast<std::int32_t>(status);
}

std::int32_t fb_http_version(void* transfer) {
    long version = 0;
    curl_easy_getinfo(static_cast<transfer_t*>(transfer)->easy, CURLINFO_HTTP_VERSION, &version);
    switch (version) {
    case CURL_HTTP_VERSION_1_0:
        return 10;
    case CURL_HTTP_VERSION_1_1:
        return 11;
    case CURL_HTTP_VERSION_2_0:
        return 20;
    case CURL_HTTP_VERSION_3:
        return 30;
    default:
        return 0;
    }
}

fb_string_t* fb_http_url(void* transfer) {
    const char* url = nullptr;
    curl_easy_getinfo(static_cast<transfer_t*>(transfer)->easy, CURLINFO_EFFECTIVE_URL, &url);
    return fb_string_from_cstring(url != nullptr ? url : "");
}

void fb_http_response_headers(void* transfer, fb_array_t* names, fb_array_t* values) {
    CURL* easy = static_cast<transfer_t*>(transfer)->easy;
    // The last response's, after the redirects, without its trailers.
    curl_header* header = nullptr;
    while ((header = curl_easy_nextheader(easy, CURLH_HEADER, -1, header)) != nullptr) {
        *static_cast<fb_string_t**>(fb_array_push(names, sizeof(fb_string_t*), nullptr)) =
            fb_string_from_cstring(header->name);
        *static_cast<fb_string_t**>(fb_array_push(values, sizeof(fb_string_t*), nullptr)) =
            fb_string_from_cstring(header->value);
    }
}

void fb_http_take_body(void* transfer, fb_array_t* output) {
    std::vector<std::uint8_t> body =
        std::exchange(static_cast<transfer_t*>(transfer)->received, {});
    fb_array_reserve(output, 1, static_cast<std::int64_t>(body.size()), nullptr);
    output->count = static_cast<std::int64_t>(body.size());
    if (!body.empty()) {
        std::memcpy(output->data, body.data(), body.size());
    }
}

fb_string_t* fb_http_message(void* transfer) {
    const auto& given = *static_cast<transfer_t*>(transfer);
    // libcurl's own words for what failed, or else its words for the code.
    if (given.error.front() != '\0') {
        return fb_string_from_cstring(given.error.data());
    }

    switch (given.outcome) {
    case OUTCOME_ENDED:
        return string_of("the program ended first");
    case OUTCOME_RESPONSE:
        return string_of("");
    default:
        return string_of("the transfer failed");
    }
}

void fb_http_free(void* transfer) {
    auto* given = static_cast<transfer_t*>(transfer);
    {
        const std::scoped_lock lock(given->mutex);
        given->abandoned = true;
    }

    // A blocking one that didn't end is taken out of its client's handle, here.
    if (given->added) {
        curl_multi_remove_handle(given->client->multi, given->easy);
        given->added = false;
    }

    release(given);
}
}
