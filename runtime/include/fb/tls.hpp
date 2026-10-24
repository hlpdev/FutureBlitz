#pragma once

#include "fb/runtime.hpp"

#include <cstdint>

// std.net.tls's TLS in the runtime, over mbedTLS, which a program links only when it calls one
// of these. A connection's records go through memory: fb_tls_feed gives it what came from the
// stream it's on, and fb_tls_take what it has to send there, which std.net.tls moves over that
// stream, blocking or Async, so the runtime never touches the stream. A configuration is a
// client's or a server's: the roots it trusts, the OS's or its own, its own certificate chain and
// key, and the application protocols it offers, set before a connection has it. Arrays and
// Strings are only borrowed.
//
// A handshake's step gives 0 once it's done, 1 when it needs more of what the other side sends,
// -1 when it failed, with why in fb_tls_message, and -2 when the stream ended first. A read gives
// how many bytes it read, 0 once the other side has closed with close_notify, -1 when it needs
// more of what the other side sends, -2 when it failed, and -3 when the stream ended without
// close_notify, so what came may be cut short.

extern "C" {

/// A new configuration, a client's, or a server's when `server` isn't 0, which trusts the OS's
/// roots; fb_tls_config_free lets go of it once its connections are done with it.
void* fb_tls_config_new(std::int32_t server);
void fb_tls_config_free(void* config);
/// Has it trust only the certificates fb_tls_config_trust adds, or the OS's roots again when
/// `own` is 0.
void fb_tls_config_trust_own(void* config, std::int32_t own);
/// Adds a certificate, as DER, to the roots it trusts of its own: 0, or -1 when mbedTLS can't
/// read it, which it then doesn't trust.
std::int32_t fb_tls_config_trust(void* config, const fb_array_t* der);
/// Has a server ask each client for a certificate that one of its roots signed, and refuse one
/// without.
void fb_tls_config_require_clients(void* config);
/// Adds a certificate, as DER, to its own chain, which starts with its own: 0, or -1 when mbedTLS
/// can't read it.
std::int32_t fb_tls_config_chain(void* config, const fb_array_t* der);
/// Its own private key, as PEM, which its chain's first certificate is for: 0, -1 when mbedTLS
/// can't read it, or -2 when it isn't that certificate's.
std::int32_t fb_tls_config_key(void* config, const fb_string_t* pem);
/// Adds an application protocol it offers through ALPN, after those it prefers.
void fb_tls_config_offer(void* config, const fb_string_t* protocol);

/// A new connection of `config`'s, which fb_tls_free lets go of: a client's to `host`, which its
/// server's certificate has to be for, or a server's, whose `host` is "".
void* fb_tls_new(void* config, const fb_string_t* host);
void fb_tls_free(void* tls);
/// Why its last step that failed did.
fb_string_t* fb_tls_message(void* tls);
/// Takes `count` bytes that came from the other side, or with 0, the end of the stream.
void fb_tls_feed(void* tls, const std::uint8_t* from, std::int64_t count);
/// How many bytes it has for the other side.
std::int64_t fb_tls_pending(void* tls);
/// Moves up to `capacity` of the bytes it has for the other side to `into`, and gives how many.
std::int64_t fb_tls_take(void* tls, std::uint8_t* into, std::int64_t capacity);
/// The handshake's next step.
std::int32_t fb_tls_handshake(void* tls);
/// Reads up to `capacity` bytes of what the other side sent into `into`.
std::int64_t fb_tls_read(void* tls, std::uint8_t* into, std::int64_t capacity);
/// Writes `count` bytes for the other side, as records fb_tls_take gives: 0, or -1 when it
/// failed.
std::int32_t fb_tls_write(void* tls, const std::uint8_t* from, std::int64_t count);
/// Writes close_notify, which tells the other side nothing more comes.
void fb_tls_close(void* tls);
/// The application protocol ALPN agreed on, or null for none.
fb_string_t* fb_tls_protocol(void* tls);
/// The version of TLS the handshake agreed on: "TLS 1.2" or "TLS 1.3".
fb_string_t* fb_tls_version(void* tls);
/// The other side's certificate, as PEM, or null when it sent none.
fb_string_t* fb_tls_peer_certificate(void* tls);
}
