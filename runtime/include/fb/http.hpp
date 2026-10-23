#pragma once

#include "fb/runtime.hpp"

#include <cstdint>

// std.net.http's client in the runtime, over libcurl and its TLS over mbedTLS, which a program
// links only when it calls one of these. A client keeps its cookies, its DNS answers, and its TLS
// sessions for each of its transfers, and its connections for its blocking ones; Async transfers
// run on the I/O thread, whose connections they share. A transfer is one request and its
// response, after the redirects it follows. Arrays and Strings are only borrowed. std.net.http
// checks what it passes first, so a bad one here is a bug, which panics.
//
// A transfer's outcome is 0 when it got a response, whatever its status, or else what failed:
// 1 a host that wasn't found, 2 a refused connection, 3 an unreachable network or host, 4 a
// connection reset, 5 one closed before the whole response came, 6 TLS, 7 a timeout, 8 another
// failure of the network or of the server, 9 a URL that can't be used, 10 too many redirects,
// and 11 a program that ended before the transfer did.

extern "C" {

/// A new client, which fb_http_client_free lets go of once its transfers are done with it.
void* fb_http_client_new();
void fb_http_client_free(void* client);
/// Forgets the cookies the client keeps.
void fb_http_client_clear_cookies(void* client);
/// Has the client's transfers trust only the certificates fb_http_client_trust adds, from then
/// on, or the OS's roots again when `own` is 0.
void fb_http_client_trust_own(void* client, std::int32_t own);
/// Adds a certificate, as DER, to those the client trusts.
void fb_http_client_trust(void* client, const fb_array_t* der);

/// A new transfer of `method`, like "GET", to `url`, through the client, which
/// fb_http_free lets go of.
void* fb_http_new(void* client, const fb_string_t* method, const fb_string_t* url);
/// Adds a request header, as its name and its value.
void fb_http_header(void* transfer, const fb_string_t* name, const fb_string_t* value);
/// The request's body.
void fb_http_body(void* transfer, const fb_array_t* body);
/// How long the whole transfer, and its connecting, may take, in milliseconds, 0 for no limit;
/// the most redirects it follows, or -1 to follow none; and whether it asks for a compressed
/// response, which it decompresses.
void fb_http_options(void* transfer, std::int64_t timeout, std::int64_t connect_timeout,
                     std::int32_t redirects, std::int32_t decompress);
/// The proxy the transfer goes through, or none for an empty one, where without it libcurl
/// takes the environment's (http_proxy, https_proxy, and no_proxy).
void fb_http_proxy(void* transfer, const fb_string_t* proxy);
/// The User-Agent header it sends unless it was given one.
void fb_http_user_agent(void* transfer, const fb_string_t* agent);

/// Runs the transfer to its end, on the calling thread, and gives its outcome.
std::int32_t fb_http_perform(void* transfer);
/// Runs the transfer on the calling thread until more of the response came, giving 0, or until
/// its end, giving its outcome plus 1.
std::int32_t fb_http_advance(void* transfer);
/// Finishes `task`, a new Task<Int32>, once the transfer on the I/O thread has ended, with its
/// outcome plus 1, or, when `progress` isn't 0, once more of the response came, with 0. The first
/// call starts it there.
void fb_http_wait_async(fb_task_t* task, void* transfer, std::int32_t progress);
/// How many bytes of the response's body came so far, and how many it said it has, or -1 when it
/// didn't say.
std::int64_t fb_http_received(void* transfer);
std::int64_t fb_http_expected(void* transfer);

/// Once the transfer is done: the response's status, the URL it came from after the redirects,
/// its headers, added to `names` and `values`, two String[]s, in its order, its body, into
/// `output`, a UInt8[], which it no longer keeps, and what failed, as a message.
std::int32_t fb_http_status(void* transfer);
fb_string_t* fb_http_url(void* transfer);
void fb_http_response_headers(void* transfer, fb_array_t* names, fb_array_t* values);
void fb_http_take_body(void* transfer, fb_array_t* output);
fb_string_t* fb_http_message(void* transfer);
/// Lets go of the transfer, stopping it if it's still under way.
void fb_http_free(void* transfer);

// What std.net.http's Headers and Body use, in the runtime's own archive, which needs no
// libcurl.

/// `bytes`, a UInt8[], as a new String, with U+FFFD for each byte that doesn't start well-formed
/// UTF-8.
fb_string_t* fb_string_from_utf8(const fb_array_t* bytes);
/// 1 when `a` and `b` are the same header name, which ASCII's letter cases don't change, or 0.
std::int32_t fb_headers_same_name(const fb_string_t* a, const fb_string_t* b);
/// 1 when `name` is a header's name, a token of RFC 9110's characters, or 0.
std::int32_t fb_headers_is_name(const fb_string_t* name);
/// 1 when `value` can be a header's value, with no control character but tabs, or 0.
std::int32_t fb_headers_is_value(const fb_string_t* value);
/// `text` with each byte but the letters, digits, '-', '.', '_', and '~' as %XX, for a form or a
/// query.
fb_string_t* fb_percent_encode(const fb_string_t* text);
}
