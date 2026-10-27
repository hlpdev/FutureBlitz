#pragma once

#include <cstdint>
#include <mbedtls/x509_crt.h>
#include <span>
#include <string>

// The certificates TLS trusts by default: the OS's roots, which are the runtime's one piece of
// per-OS code for TLS. std.net.http's client gives them to libcurl.
namespace fb::runtime {

/// The OS's trusted roots, as PEM, read once, the first time: Windows' ROOT store, each that's
/// for servers, valid now, and trusted by Windows' SSL policy for servers, so none Windows
/// distrusts, as from a date, macOS's system anchors, all of them, whose dates mbedTLS checks as
/// it verifies, or elsewhere the first of the bundle files distributions keep that's there, or
/// the one SSL_CERT_FILE names. Empty when there are none.
[[nodiscard]] const std::string& os_roots();

/// mbedTLS's check of each certificate of a chain it verifies against os_roots(), its f_vrfy.
/// Windows' ROOT store holds only the roots Windows has fetched so far, from the list it trusts,
/// so on Windows a chain whose one flaw is that none of os_roots() signed it is checked again as
/// Windows checks a server's for Schannel, which fetches a root it trusts that the store hasn't
/// got yet; elsewhere, or for any other flaw, it changes nothing. `clients` is null for a
/// client's check of a server, and anything else for a server's check of its clients.
int os_verify(void* clients, mbedtls_x509_crt* certificate, int depth, std::uint32_t* flags);

/// Adds `der`, a certificate, to `pem` as a PEM block.
void append_pem(std::string& pem, std::span<const std::uint8_t> der);

} // namespace fb::runtime
