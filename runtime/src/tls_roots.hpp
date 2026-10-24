#pragma once

#include <cstdint>
#include <span>
#include <string>

// The certificates TLS trusts by default: the OS's roots, which are the runtime's one piece of
// per-OS code for TLS. std.net.http's client gives them to libcurl.
namespace fb::runtime {

/// The OS's trusted roots, as PEM, read once, the first time: Windows' ROOT store, each that's
/// for servers and valid now, macOS's system anchors, all of them, whose dates mbedTLS checks as
/// it verifies, or elsewhere the first of the bundle files distributions keep that's there, or
/// the one SSL_CERT_FILE names. Empty when there are none.
[[nodiscard]] const std::string& os_roots();

/// Adds `der`, a certificate, to `pem` as a PEM block.
void append_pem(std::string& pem, std::span<const std::uint8_t> der);

} // namespace fb::runtime
