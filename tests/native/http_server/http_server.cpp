// The HTTP server std.net.http's program tests run against, with no internet: plain sockets,
// HTTP/1.1 with keep-alive, TLS through mbedTLS with the test certificates beside it, HTTP/2
// through nghttp2 where TLS's ALPN agrees on it, and bodies compressed by zlib-ng and zstd. It
// listens on 127.0.0.1 for HTTP, for HTTPS, and for HTTPS that offers only HTTP/1.1, keeps a
// fourth port bound but not listening, which refuses connections, and runs the program it's
// given. The program's input is the HTTP, HTTPS, and refused URLs, a line each, then the HTTPS
// URL that offers only HTTP/1.1, then the test CA's PEM, then what this server's own input was.
// It exits as the program does, with its exit code; the program's output is its own.
//
//     fb_http_test_server <program>
//
// Each path it answers is in answer(), below.

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <memory>
#include <nghttp2/nghttp2.h>
#include <optional>
#include <psa/crypto.h>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <zlib.h>
#include <zstd.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace {

#ifdef _WIN32
using socket_t = SOCKET;
const socket_t no_socket = INVALID_SOCKET;

void close_socket(const socket_t socket) {
    closesocket(socket);
}
#else
using socket_t = int;
const socket_t no_socket = -1;

void close_socket(const socket_t socket) {
    close(socket);
}
#endif

[[noreturn]] void fail(const char* what) {
    std::fprintf(stderr, "fb_http_test_server: %s\n", what);
    std::fflush(stderr);
    std::_Exit(2);
}

std::string read_file(const char* path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        fail("a test certificate is missing");
    }

    return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}

/// A connection, plain or TLS, that reads and writes bytes.
class connection_t {
public:
    explicit connection_t(const socket_t socket) : m_socket(socket) {}
    connection_t(const connection_t&) = delete;
    connection_t& operator=(const connection_t&) = delete;
    virtual ~connection_t() {
        close_socket(m_socket);
    }

    /// Up to `size` bytes, 0 at the end, or below 0 when reading failed.
    virtual std::ptrdiff_t read(char* data, std::size_t size) = 0;
    /// Whether all of `data` was written.
    virtual bool write(std::string_view data) = 0;

    /// Ends the connection with a reset, as a crash would.
    void reset() {
        linger stop{};
        stop.l_onoff = 1;
        stop.l_linger = 0;
        setsockopt(m_socket, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&stop),
                   sizeof(stop));
    }

    /// Ends the connection as a server that's done does: it sends no more, and what the client
    /// still sends is read and dropped till it closes too, so the close is no reset.
    void finish() {
#ifdef _WIN32
        shutdown(m_socket, SD_SEND);
#else
        shutdown(m_socket, SHUT_WR);
#endif
        std::array<char, 4096> rest{};
        while (recv(m_socket, rest.data(), static_cast<int>(rest.size()), 0) > 0) {}
    }

protected:
    socket_t m_socket;
};

class plain_t : public connection_t {
public:
    using connection_t::connection_t;

    std::ptrdiff_t read(char* data, const std::size_t size) override {
        return recv(m_socket, data, static_cast<int>(size), 0);
    }

    bool write(std::string_view data) override {
        while (!data.empty()) {
            const auto sent = send(m_socket, data.data(), static_cast<int>(data.size()), 0);
            if (sent <= 0) {
                return false;
            }

            data.remove_prefix(static_cast<std::size_t>(sent));
        }

        return true;
    }
};

/// A TLS port's setup, which each of its connections shares, with the protocols its ALPN offers.
struct tls_config_t {
    mbedtls_ssl_config config{};
    mbedtls_x509_crt chain{};
    mbedtls_pk_context key{};
    std::array<const char*, 3> protocols{};
};

class tls_t : public connection_t {
public:
    tls_t(const socket_t socket, const tls_config_t& config) : connection_t(socket) {
        mbedtls_ssl_init(&m_ssl);
        m_ready = mbedtls_ssl_setup(&m_ssl, &config.config) == 0;
        mbedtls_ssl_set_bio(&m_ssl, &m_socket, send_some, receive_some, nullptr);
    }

    tls_t(const tls_t&) = delete;
    tls_t& operator=(const tls_t&) = delete;

    ~tls_t() override {
        mbedtls_ssl_close_notify(&m_ssl);
        mbedtls_ssl_free(&m_ssl);
    }

    /// Whether the handshake went through.
    bool handshake() {
        if (!m_ready) {
            return false;
        }

        int result = 0;
        while ((result = mbedtls_ssl_handshake(&m_ssl)) != 0) {
            if (result != MBEDTLS_ERR_SSL_WANT_READ && result != MBEDTLS_ERR_SSL_WANT_WRITE) {
                return false;
            }
        }

        return true;
    }

    /// Whether the handshake's ALPN agreed on HTTP/2.
    [[nodiscard]] bool http2() const {
        const char* protocol = mbedtls_ssl_get_alpn_protocol(&m_ssl);
        return protocol != nullptr && std::string_view(protocol) == "h2";
    }

    std::ptrdiff_t read(char* data, const std::size_t size) override {
        while (true) {
            const int got = mbedtls_ssl_read(&m_ssl, reinterpret_cast<unsigned char*>(data), size);
            if (got == MBEDTLS_ERR_SSL_WANT_READ || got == MBEDTLS_ERR_SSL_WANT_WRITE ||
                got == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) {
                continue;
            }

            if (got == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
                return 0;
            }

            return got;
        }
    }

    bool write(std::string_view data) override {
        while (!data.empty()) {
            const int sent = mbedtls_ssl_write(
                &m_ssl, reinterpret_cast<const unsigned char*>(data.data()), data.size());
            if (sent == MBEDTLS_ERR_SSL_WANT_READ || sent == MBEDTLS_ERR_SSL_WANT_WRITE) {
                continue;
            }

            if (sent <= 0) {
                return false;
            }

            data.remove_prefix(static_cast<std::size_t>(sent));
        }

        return true;
    }

private:
    mbedtls_ssl_context m_ssl{};
    bool m_ready = false;

    static int send_some(void* socket, const unsigned char* data, const std::size_t size) {
        const auto sent = send(*static_cast<socket_t*>(socket), reinterpret_cast<const char*>(data),
                               static_cast<int>(size), 0);
        return sent < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : static_cast<int>(sent);
    }

    static int receive_some(void* socket, unsigned char* data, const std::size_t size) {
        const auto got = recv(*static_cast<socket_t*>(socket), reinterpret_cast<char*>(data),
                              static_cast<int>(size), 0);
        return got < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : static_cast<int>(got);
    }
};

std::string lower(std::string_view text) {
    std::string lowered(text);
    std::ranges::transform(lowered, lowered.begin(), [](const unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return lowered;
}

struct request_t {
    std::string method;
    std::string target;
    /// Each header's name, lowered, and its value.
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;

    [[nodiscard]] std::optional<std::string> header(const std::string_view name) const {
        for (const auto& [key, value] : headers) {
            if (key == name) {
                return value;
            }
        }

        return std::nullopt;
    }

    /// The target's path, without its query.
    [[nodiscard]] std::string_view path() const {
        const std::string_view whole = target;
        return whole.substr(0, whole.find('?'));
    }

    /// The query's value of `name`, or nothing.
    [[nodiscard]] std::optional<std::string> query(const std::string_view name) const {
        const std::size_t at = target.find('?');
        if (at == std::string::npos) {
            return std::nullopt;
        }

        std::string_view rest = std::string_view(target).substr(at + 1);
        while (!rest.empty()) {
            const std::string_view pair = rest.substr(0, rest.find('&'));
            rest.remove_prefix(std::min(rest.size(), pair.size() + 1));
            const std::size_t equals = pair.find('=');
            if (pair.substr(0, equals) == name) {
                return std::string(equals == std::string_view::npos ? "" : pair.substr(equals + 1));
            }
        }

        return std::nullopt;
    }
};

/// Reads the next request from the connection, with what came after the last kept in `buffer`,
/// or nothing at its end.
std::optional<request_t> read_request(connection_t& connection, std::string& buffer) {
    std::array<char, 16384> chunk{};
    const auto more = [&] {
        const std::ptrdiff_t got = connection.read(chunk.data(), chunk.size());
        if (got <= 0) {
            return false;
        }

        buffer.append(chunk.data(), static_cast<std::size_t>(got));
        return true;
    };

    std::size_t end = 0;
    while ((end = buffer.find("\r\n\r\n")) == std::string::npos) {
        if (!more()) {
            return std::nullopt;
        }
    }

    request_t request;
    std::string_view head = std::string_view(buffer).substr(0, end);
    const std::string_view line = head.substr(0, head.find("\r\n"));
    request.method = line.substr(0, line.find(' '));
    const std::string_view rest = line.substr(std::min(line.size(), request.method.size() + 1));
    request.target = rest.substr(0, rest.find(' '));
    head.remove_prefix(std::min(head.size(), line.size() + 2));
    while (!head.empty()) {
        const std::string_view field = head.substr(0, head.find("\r\n"));
        head.remove_prefix(std::min(head.size(), field.size() + 2));
        const std::size_t colon = field.find(':');
        if (colon == std::string_view::npos) {
            continue;
        }

        std::string_view value = field.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
            value.remove_prefix(1);
        }

        request.headers.emplace_back(lower(field.substr(0, colon)), value);
    }

    buffer.erase(0, end + 4);
    if (lower(request.header("expect").value_or("")) == "100-continue" &&
        !connection.write("HTTP/1.1 100 Continue\r\n\r\n")) {
        return std::nullopt;
    }

    const std::size_t length = std::stoull(request.header("content-length").value_or("0"));
    while (buffer.size() < length) {
        if (!more()) {
            return std::nullopt;
        }
    }

    request.body = buffer.substr(0, length);
    buffer.erase(0, length);
    return request;
}

struct response_t {
    int status = 200;
    std::string reason = "OK";
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    /// Sent in chunks of this many bytes, when it isn't 0.
    std::size_t chunks = 0;
    /// Its Content-Length, when it isn't the body's.
    std::optional<std::size_t> length;
    /// How much of the body comes before a pause of 300 ms, and the rest after, when it isn't 0.
    std::size_t pause = 0;
};

response_t text(std::string body, const int status = 200, std::string reason = "OK") {
    return {
        .status = status,
        .reason = std::move(reason),
        .headers = { { "Content-Type", "text/plain; charset=utf-8" } },
        .body = std::move(body),
    };
}

bool send_response(connection_t& connection, const response_t& response, const bool head) {
    std::string out =
        "HTTP/1.1 " + std::to_string(response.status) + " " + response.reason + "\r\n";
    for (const auto& [name, value] : response.headers) {
        out += name + ": " + value + "\r\n";
    }

    if (response.chunks != 0) {
        out += "Transfer-Encoding: chunked\r\n\r\n";
        if (head) {
            return connection.write(out);
        }

        for (std::size_t at = 0; at < response.body.size(); at += response.chunks) {
            const std::string_view part =
                std::string_view(response.body).substr(at, response.chunks);
            std::array<char, 32> size{};
            std::snprintf(size.data(), size.size(), "%zx\r\n", part.size());
            out += size.data();
            out += part;
            out += "\r\n";
        }

        out += "0\r\n\r\n";
        return connection.write(out);
    }

    out += "Content-Length: " + std::to_string(response.length.value_or(response.body.size())) +
           "\r\n\r\n";
    if (head) {
        return connection.write(out);
    }

    const std::string_view body = response.body;
    out += body.substr(0, response.pause != 0 ? response.pause : body.size());
    if (!connection.write(out)) {
        return false;
    }

    if (response.pause == 0) {
        return true;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    return connection.write(body.substr(std::min(body.size(), response.pause)));
}

/// `text` compressed as the HTTP coding `coding` is: gzip's and deflate's (zlib's format, as HTTP's
/// deflate is) by zlib, and zstd's.
std::string compressed(const std::string_view coding, const std::string& text) {
    if (coding == "zstd") {
        std::string out(ZSTD_compressBound(text.size()), '\0');
        out.resize(ZSTD_compress(out.data(), out.size(), text.data(), text.size(), 3));
        return out;
    }

    z_stream stream{};
    deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, coding == "gzip" ? 31 : 15, 8,
                 Z_DEFAULT_STRATEGY);
    std::string out(deflateBound(&stream, static_cast<uLong>(text.size())), '\0');
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(text.data()));
    stream.avail_in = static_cast<uInt>(text.size());
    stream.next_out = reinterpret_cast<Bytef*>(out.data());
    stream.avail_out = static_cast<uInt>(out.size());
    deflate(&stream, Z_FINISH);
    out.resize(stream.total_out);
    deflateEnd(&stream);
    return out;
}

/// What the connection does after a response.
enum class after_t : std::uint8_t { KEEP, CLOSE, RESET };

/// Each path's answer:
///
/// - /hello: "hello"; /utf8: text that isn't all well-formed UTF-8.
/// - /echo: the request's method, target, Content-Type, and body, a line each but the body.
/// - /header/<name>: the request's values of that header, with " | " between them, or "(none)".
/// - /status/<code>: that status.
/// - /redirect/<n>: a 302 to /redirect/<n - 1>, and /redirect/0 "redirected".
/// - /moved/<code>: a <code> redirect to /echo; /redirect-to?url=<url>: a 302 to the URL.
/// - /redirect-with-body: a 302 to /bytes/2 with a body of its own, 5000 bytes, of which the
///   second half comes 300 ms after the first over HTTP/1.1.
/// - /cookies/set?name=<n>&value=<v>: sets the cookie; /cookies: the request's Cookie header.
/// - /bytes/<n>: n bytes, each its index's remainder by 251, and with ?pause=<k>, the first k of
///   them 300 ms before the rest over HTTP/1.1; /chunked: "chunk" ten times, in chunks of 7
///   bytes.
/// - /multi: two X-Multi headers.
/// - /compressed/<coding>: "<coding>" and "fox " 5000 times, compressed by the coding (gzip,
///   deflate, or zstd) when the request's Accept-Encoding has it.
/// - /slow: "slow" after 3 seconds.
/// - /close: no response, closed; /reset: no response, reset; /truncated: 5 of the 100 bytes it
///   says.
/// - An absolute URL, as a proxy is sent: "proxied <path>".
std::pair<response_t, after_t> answer(const request_t& request) {
    const std::string_view path = request.path();
    const auto starts = [&](const std::string_view prefix) { return path.starts_with(prefix); };
    const auto after = [&](const std::string_view prefix) {
        return std::string(path.substr(prefix.size()));
    };
    if (request.target.starts_with("http://")) {
        const std::string_view target = request.target;
        const std::size_t slash = target.find('/', std::string_view("http://").size());
        return { text("proxied " + std::string(target.substr(std::min(target.size(), slash)))),
                 after_t::KEEP };
    }

    if (path == "/hello") {
        return { text("hello"), after_t::KEEP };
    }

    if (path == "/utf8") {
        return { text("h\xC3\xA9llo \xE2\x98\x83 \xFF!"), after_t::KEEP };
    }

    if (path == "/echo") {
        return { text(request.method + "\n" + request.target + "\n" +
                      request.header("content-type").value_or("(none)") + "\n" + request.body),
                 after_t::KEEP };
    }

    if (starts("/header/")) {
        const std::string name = lower(after("/header/"));
        std::string values;
        for (const auto& [key, value] : request.headers) {
            if (key == name) {
                values += values.empty() ? value : " | " + value;
            }
        }

        return { text(values.empty() ? "(none)" : values), after_t::KEEP };
    }

    if (starts("/status/")) {
        const int status = std::stoi(after("/status/"));
        return { text("status " + std::to_string(status), status, "Status"), after_t::KEEP };
    }

    if (starts("/redirect/")) {
        const int left = std::stoi(after("/redirect/"));
        if (left == 0) {
            return { text("redirected"), after_t::KEEP };
        }

        response_t moved = text("", 302, "Found");
        moved.headers.emplace_back("Location", "/redirect/" + std::to_string(left - 1));
        return { moved, after_t::KEEP };
    }

    if (starts("/moved/")) {
        const int status = std::stoi(after("/moved/"));
        response_t moved = text("", status, "Moved");
        moved.headers.emplace_back("Location", "/echo");
        return { moved, after_t::KEEP };
    }

    if (path == "/redirect-to") {
        response_t moved = text("", 302, "Found");
        moved.headers.emplace_back("Location", request.query("url").value_or("/"));
        return { moved, after_t::KEEP };
    }

    if (path == "/redirect-with-body") {
        response_t moved = text(std::string(5000, 'x'), 302, "Found");
        moved.headers.emplace_back("Location", "/bytes/2");
        moved.pause = 2500;
        return { moved, after_t::KEEP };
    }

    if (path == "/cookies/set") {
        response_t set = text("set");
        set.headers.emplace_back("Set-Cookie", request.query("name").value_or("") + "=" +
                                                   request.query("value").value_or("") +
                                                   "; Path=/");
        return { set, after_t::KEEP };
    }

    if (path == "/cookies") {
        return { text(request.header("cookie").value_or("(none)")), after_t::KEEP };
    }

    if (starts("/bytes/")) {
        const std::size_t count = std::stoull(after("/bytes/"));
        response_t bytes = text("");
        bytes.headers = { { "Content-Type", "application/octet-stream" } };
        for (std::size_t i = 0; i < count; ++i) {
            bytes.body += static_cast<char>(i % 251);
        }

        bytes.pause = std::stoull(request.query("pause").value_or("0"));
        return { bytes, after_t::KEEP };
    }

    if (path == "/chunked") {
        response_t chunked = text("");
        for (int i = 0; i < 10; ++i) {
            chunked.body += "chunk";
        }

        chunked.chunks = 7;
        return { chunked, after_t::KEEP };
    }

    if (starts("/compressed/")) {
        const std::string coding = after("/compressed/");
        response_t response = text(coding);
        for (int i = 0; i < 5000; ++i) {
            response.body += " fox";
        }

        if (request.header("accept-encoding").value_or("").contains(coding)) {
            response.body = compressed(coding, response.body);
            response.headers.emplace_back("Content-Encoding", coding);
        }

        return { response, after_t::KEEP };
    }

    if (path == "/multi") {
        response_t multi = text("multi");
        multi.headers.emplace_back("X-Multi", "one");
        multi.headers.emplace_back("X-Multi", "two");
        return { multi, after_t::KEEP };
    }

    if (path == "/slow") {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        return { text("slow"), after_t::KEEP };
    }

    if (path == "/close") {
        return { {}, after_t::CLOSE };
    }

    if (path == "/reset") {
        return { {}, after_t::RESET };
    }

    if (path == "/truncated") {
        response_t truncated = text("trunc");
        truncated.length = 100;
        return { truncated, after_t::CLOSE };
    }

    return { text("no such path", 404, "Not Found"), after_t::KEEP };
}

void serve(std::unique_ptr<connection_t> connection) {
    std::string buffer;
    while (const std::optional<request_t> request = read_request(*connection, buffer)) {
        auto [response, then] = answer(*request);
        if (then == after_t::RESET) {
            connection->reset();
            return;
        }

        // A response to close after is only the truncated one.
        if (then == after_t::CLOSE) {
            if (response.length) {
                send_response(*connection, response, false);
            }

            connection->finish();
            return;
        }

        if (!send_response(*connection, response, request->method == "HEAD")) {
            return;
        }
    }
}

/// A request that came on an HTTP/2 stream, and the body of the response it's sent there.
struct stream_t {
    request_t request;
    std::string body;
    std::size_t sent = 0;
    /// Whether the body's end ends the stream, as it does but for the truncated one.
    bool ends = true;
};

/// An HTTP/2 connection's session and streams, which nghttp2's callbacks are given.
struct http2_t {
    nghttp2_session* session = nullptr;
    std::map<std::int32_t, stream_t> streams;
    /// What the connection does once what it has to send is sent.
    after_t then = after_t::KEEP;
};

http2_t& http2_of(void* data) {
    return *static_cast<http2_t*>(data);
}

bool has_flag(const std::uint8_t flags, const int flag) {
    return (static_cast<unsigned>(flags) & static_cast<unsigned>(flag)) != 0;
}

/// The next part of a response's body, as nghttp2 asks for it, whose callbacks take a source
/// that isn't const.
nghttp2_ssize next_part(nghttp2_session* /*session*/, const std::int32_t /*id*/,
                        std::uint8_t* buffer, const std::size_t length, std::uint32_t* flags,
                        nghttp2_data_source* source, // NOLINT(misc-const-correctness)
                        void* /*data*/) {
    stream_t& stream = *static_cast<stream_t*>(source->ptr);
    const std::size_t count = std::min(length, stream.body.size() - stream.sent);
    std::copy_n(stream.body.data() + stream.sent, count, buffer);
    stream.sent += count;
    if (stream.sent == stream.body.size()) {
        *flags |= static_cast<std::uint32_t>(NGHTTP2_DATA_FLAG_EOF);
        if (!stream.ends) {
            *flags |= static_cast<std::uint32_t>(NGHTTP2_DATA_FLAG_NO_END_STREAM);
        }
    }

    return static_cast<nghttp2_ssize>(count);
}

/// Answers the request that came whole on stream `id`, as serve() answers one over HTTP/1.1.
void respond(http2_t& connection, const std::int32_t id) {
    const auto found = connection.streams.find(id);
    if (found == connection.streams.end()) {
        return;
    }

    stream_t& stream = found->second;
    auto [response, then] = answer(stream.request);
    // A response to close after is only the truncated one, whose stream doesn't end.
    if (then == after_t::RESET || (then == after_t::CLOSE && !response.length)) {
        connection.then = then;
        return;
    }

    if (then == after_t::CLOSE) {
        connection.then = then;
        stream.ends = false;
    }

    std::vector<std::pair<std::string, std::string>> fields{
        { ":status", std::to_string(response.status) },
    };
    for (const auto& [name, value] : response.headers) {
        fields.emplace_back(lower(name), value);
    }

    fields.emplace_back("content-length",
                        std::to_string(response.length.value_or(response.body.size())));
    std::vector<nghttp2_nv> headers;
    headers.reserve(fields.size());
    for (auto& [name, value] : fields) {
        headers.push_back({
            .name = reinterpret_cast<std::uint8_t*>(name.data()),
            .value = reinterpret_cast<std::uint8_t*>(value.data()),
            .namelen = name.size(),
            .valuelen = value.size(),
            .flags = NGHTTP2_NV_FLAG_NONE,
        });
    }

    stream.body = std::move(response.body);
    const nghttp2_data_provider2 body{ .source = { .ptr = &stream }, .read_callback = next_part };
    nghttp2_submit_response2(connection.session, id, headers.data(), headers.size(),
                             stream.request.method == "HEAD" ? nullptr : &body);
}

int on_begin_headers(nghttp2_session* /*session*/, const nghttp2_frame* frame, void* data) {
    if (frame->hd.type == NGHTTP2_HEADERS && frame->headers.cat == NGHTTP2_HCAT_REQUEST) {
        http2_of(data).streams.try_emplace(frame->hd.stream_id);
    }

    return 0;
}

/// A request's header, kept as HTTP/1.1's are: its :authority is its Host, and its Cookie
/// headers are one, which HTTP/2 lets a client send apart.
int on_header(nghttp2_session* /*session*/, const nghttp2_frame* frame, const std::uint8_t* name,
              const std::size_t name_length, const std::uint8_t* value,
              const std::size_t value_length, std::uint8_t /*flags*/, void* data) {
    auto& streams = http2_of(data).streams;
    const auto stream = streams.find(frame->hd.stream_id);
    if (stream == streams.end()) {
        return 0;
    }

    request_t& request = stream->second.request;
    const std::string_view key(reinterpret_cast<const char*>(name), name_length);
    std::string text(reinterpret_cast<const char*>(value), value_length);
    if (key == ":method") {
        request.method = std::move(text);
    } else if (key == ":path") {
        request.target = std::move(text);
    } else if (key == ":authority") {
        request.headers.emplace_back("host", std::move(text));
    } else if (key == "cookie" && request.header("cookie")) {
        for (auto& [field, cookies] : request.headers) {
            if (field == "cookie") {
                cookies += "; " + text;
                break;
            }
        }
    } else if (!key.starts_with(':')) {
        request.headers.emplace_back(key, std::move(text));
    }

    return 0;
}

int on_data(nghttp2_session* /*session*/, std::uint8_t /*flags*/, const std::int32_t id,
            const std::uint8_t* data, const std::size_t length, void* connection) {
    auto& streams = http2_of(connection).streams;
    if (const auto stream = streams.find(id); stream != streams.end()) {
        stream->second.request.body.append(reinterpret_cast<const char*>(data), length);
    }

    return 0;
}

int on_frame(nghttp2_session* /*session*/, const nghttp2_frame* frame, void* data) {
    const bool request = frame->hd.type == NGHTTP2_HEADERS || frame->hd.type == NGHTTP2_DATA;
    if (request && has_flag(frame->hd.flags, NGHTTP2_FLAG_END_STREAM)) {
        respond(http2_of(data), frame->hd.stream_id);
    }

    return 0;
}

int on_close(nghttp2_session* /*session*/, const std::int32_t id, std::uint32_t /*error*/,
             void* data) {
    http2_of(data).streams.erase(id);
    return 0;
}

/// Serves a connection whose TLS agreed on HTTP/2 through an nghttp2 session, answering each
/// stream's request as serve() answers HTTP/1.1's, one after another.
void serve_http2(connection_t& connection) {
    nghttp2_session_callbacks* callbacks = nullptr;
    if (nghttp2_session_callbacks_new(&callbacks) != 0) {
        return;
    }

    nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, on_begin_headers);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, on_header);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, on_data);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, on_frame);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, on_close);
    http2_t state;
    const int made = nghttp2_session_server_new(&state.session, callbacks, &state);
    nghttp2_session_callbacks_del(callbacks);
    if (made != 0) {
        return;
    }

    const nghttp2_settings_entry streams{
        .settings_id = NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS,
        .value = 100,
    };
    nghttp2_submit_settings(state.session, NGHTTP2_FLAG_NONE, &streams, 1);
    std::array<char, 16384> chunk{};
    while (true) {
        // All it has to send, then what comes next.
        const std::uint8_t* data = nullptr;
        nghttp2_ssize size = 0;
        bool written = true;
        while (written && (size = nghttp2_session_mem_send2(state.session, &data)) > 0) {
            written = connection.write(
                { reinterpret_cast<const char*>(data), static_cast<std::size_t>(size) });
        }

        if (!written || size < 0) {
            break;
        }

        if (state.then == after_t::RESET) {
            connection.reset();
            break;
        }

        if (state.then == after_t::CLOSE) {
            connection.finish();
            break;
        }

        if (nghttp2_session_want_read(state.session) == 0 &&
            nghttp2_session_want_write(state.session) == 0) {
            break;
        }

        const std::ptrdiff_t got = connection.read(chunk.data(), chunk.size());
        if (got <= 0 || nghttp2_session_mem_recv2(
                            state.session, reinterpret_cast<const std::uint8_t*>(chunk.data()),
                            static_cast<std::size_t>(got)) < 0) {
            break;
        }
    }

    nghttp2_session_del(state.session);
}

/// A socket bound to a port of 127.0.0.1 that no other program gets, listening or not.
socket_t bound(const bool listening, std::uint16_t& port) {
    const socket_t socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == no_socket) {
        fail("can't make a socket");
    }

#ifdef _WIN32
    // The program it runs doesn't get it.
    SetHandleInformation(reinterpret_cast<HANDLE>(socket), HANDLE_FLAG_INHERIT, 0);
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        (listening && listen(socket, 64) != 0)) {
        fail("can't listen on 127.0.0.1");
    }

    socklen_t size = sizeof(address);
    getsockname(socket, reinterpret_cast<sockaddr*>(&address), &size);
    port = ntohs(address.sin_port);
    return socket;
}

/// Accepts each connection on `listener`, serving each on a thread of its own.
void accept_all(const socket_t listener, const tls_config_t* tls) {
    while (true) {
        const socket_t socket = accept(listener, nullptr, nullptr);
        if (socket == no_socket) {
            continue;
        }

        std::thread([socket, tls] {
            if (tls == nullptr) {
                serve(std::make_unique<plain_t>(socket));
                return;
            }

            auto connection = std::make_unique<tls_t>(socket, *tls);
            if (!connection->handshake()) {
                return;
            }

            if (connection->http2()) {
                serve_http2(*connection);
                return;
            }

            serve(std::move(connection));
        }).detach();
    }
}

/// Sets up a TLS port with the test certificate, whose ALPN offers HTTP/2 before HTTP/1.1 when
/// `http2`, and HTTP/1.1 alone otherwise.
void set_up_tls(tls_config_t& tls, const bool http2) {
    if (psa_crypto_init() != PSA_SUCCESS) {
        fail("PSA doesn't start");
    }

    tls.protocols = http2 ? std::array<const char*, 3>{ "h2", "http/1.1", nullptr }
                          : std::array<const char*, 3>{ "http/1.1", nullptr, nullptr };

    mbedtls_ssl_config_init(&tls.config);
    mbedtls_x509_crt_init(&tls.chain);
    mbedtls_pk_init(&tls.key);
    const std::string chain = read_file(FB_HTTP_TEST_CERTIFICATE);
    const std::string key = read_file(FB_HTTP_TEST_KEY);
    if (mbedtls_x509_crt_parse(&tls.chain, reinterpret_cast<const unsigned char*>(chain.c_str()),
                               chain.size() + 1) != 0 ||
        mbedtls_pk_parse_key(&tls.key, reinterpret_cast<const unsigned char*>(key.c_str()),
                             key.size() + 1, nullptr, 0) != 0 ||
        mbedtls_ssl_config_defaults(&tls.config, MBEDTLS_SSL_IS_SERVER,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0 ||
        mbedtls_ssl_conf_own_cert(&tls.config, &tls.chain, &tls.key) != 0 ||
        mbedtls_ssl_conf_alpn_protocols(&tls.config, tls.protocols.data()) != 0) {
        fail("the test certificate doesn't load");
    }
}

/// Runs `program` with `input` as its stdin and this process's stdout and stderr, and gives its
/// exit code: 128 plus the signal's number for one a signal ended.
int run(const char* program, const std::string& input) {
#ifdef _WIN32
    SECURITY_ATTRIBUTES security{
        .nLength = sizeof(SECURITY_ATTRIBUTES),
        .lpSecurityDescriptor = nullptr,
        .bInheritHandle = TRUE,
    };
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (CreatePipe(&read_end, &write_end, &security, 0) == 0) {
        fail("can't make a pipe");
    }

    SetHandleInformation(write_end, HANDLE_FLAG_INHERIT, 0);
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE error = GetStdHandle(STD_ERROR_HANDLE);
    SetHandleInformation(output, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    SetHandleInformation(error, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = read_end;
    startup.hStdOutput = output;
    startup.hStdError = error;
    PROCESS_INFORMATION process{};
    std::string command = "\"" + std::string(program) + "\"";
    if (CreateProcessA(program, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                       &startup, &process) == 0) {
        fail("can't start the program");
    }

    CloseHandle(read_end);
    std::thread writer([&] {
        DWORD written = 0;
        WriteFile(write_end, input.data(), static_cast<DWORD>(input.size()), &written, nullptr);
        CloseHandle(write_end);
    });
    WaitForSingleObject(process.hProcess, INFINITE);
    writer.join();
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(code);
#else
    std::array<int, 2> ends{};
    if (pipe(ends.data()) != 0) {
        fail("can't make a pipe");
    }

    posix_spawn_file_actions_t actions{};
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, ends[0], STDIN_FILENO);
    posix_spawn_file_actions_addclose(&actions, ends[0]);
    posix_spawn_file_actions_addclose(&actions, ends[1]);
    std::array<char*, 2> arguments{ const_cast<char*>(program), nullptr };
    pid_t child = 0;
    if (posix_spawn(&child, program, &actions, nullptr, arguments.data(), environ) != 0) {
        fail("can't start the program");
    }

    posix_spawn_file_actions_destroy(&actions);
    close(ends[0]);
    std::thread writer([&] {
        std::string_view left = input;
        while (!left.empty()) {
            const ssize_t written = ::write(ends[1], left.data(), left.size());
            if (written <= 0) {
                break;
            }

            left.remove_prefix(static_cast<std::size_t>(written));
        }

        close(ends[1]);
    });
    int status = 0;
    waitpid(child, &status, 0);
    writer.join();
    return WIFSIGNALED(status) ? 128 + WTERMSIG(status) : WEXITSTATUS(status);
#endif
}

} // namespace

int main(const int count, char** arguments) {
    if (count != 2) {
        fail("usage: fb_http_test_server <program>");
    }

#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        fail("Winsock doesn't start");
    }
#else
    // A client gone midway is no reason to stop.
    std::signal(SIGPIPE, SIG_IGN);
#endif

    static tls_config_t tls;
    static tls_config_t tls1;
    set_up_tls(tls, true);
    set_up_tls(tls1, false);
    std::uint16_t plain_port = 0;
    std::uint16_t tls_port = 0;
    std::uint16_t refused_port = 0;
    std::uint16_t tls1_port = 0;
    const socket_t plain = bound(true, plain_port);
    const socket_t secure = bound(true, tls_port);
    const socket_t refusing = bound(false, refused_port);
    const socket_t secure1 = bound(true, tls1_port);
    std::thread(accept_all, plain, nullptr).detach();
    std::thread(accept_all, secure, &tls).detach();
    std::thread(accept_all, secure1, &tls1).detach();

    // The program's input: the URLs, the CA, and then this server's own input.
    std::string input = "http://127.0.0.1:" + std::to_string(plain_port) + "\n" +
                        "https://127.0.0.1:" + std::to_string(tls_port) + "\n" +
                        "http://127.0.0.1:" + std::to_string(refused_port) + "\n" +
                        "https://127.0.0.1:" + std::to_string(tls1_port) + "\n" +
                        read_file(FB_HTTP_TEST_CA);
    std::array<char, 4096> chunk{};
    std::size_t got = 0;
    while ((got = std::fread(chunk.data(), 1, chunk.size(), stdin)) > 0) {
        input.append(chunk.data(), got);
    }

    const int code = run(arguments[1], input);
    close_socket(refusing);
    // The threads still serving go with the process.
    std::fflush(nullptr);
    std::_Exit(code);
}
