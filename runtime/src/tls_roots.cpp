// The OS's trusted roots, for TLS: the certificates in Windows' ROOT store, macOS's system
// anchors, or a distribution's bundle file, as PEM, which mbedTLS reads, and on Windows, the
// check of a chain none of them signed as Windows checks it.

#include "tls_roots.hpp"

#include <cstddef>
#include <cstdint>
#include <mbedtls/x509.h>
#include <mutex>
#include <span>
#include <string>
#include <string_view>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// windows.h first, which wincrypt.h needs.
#include <cstring>
#include <utility>
#include <vector>
#include <wincrypt.h>
#elifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#else
#include <array>
#include <cstdlib>
#include <fstream>
#include <ios>
#include <iterator>
#endif

namespace {

#ifdef _WIN32

/// Whether `certificate` can vouch for servers: it says nothing of what it's for, which means
/// anything, or it says server authentication.
bool for_servers(PCCERT_CONTEXT certificate) {
    DWORD size = 0;
    if (CertGetEnhancedKeyUsage(certificate, 0, nullptr, &size) == FALSE) {
        return GetLastError() == static_cast<DWORD>(CRYPT_E_NOT_FOUND);
    }

    std::vector<std::byte> buffer(size);
    auto* usage = reinterpret_cast<PCERT_ENHKEY_USAGE>(buffer.data());
    if (CertGetEnhancedKeyUsage(certificate, 0, usage, &size) == FALSE) {
        return false;
    }

    // None listed is for any use when Windows found no usage to list, and for none otherwise.
    if (usage->cUsageIdentifier == 0) {
        return GetLastError() == static_cast<DWORD>(CRYPT_E_NOT_FOUND);
    }

    for (DWORD i = 0; i < usage->cUsageIdentifier; ++i) {
        if (std::strcmp(usage->rgpszUsageIdentifier[i], szOID_PKIX_KP_SERVER_AUTH) == 0) {
            return true;
        }
    }

    return false;
}

/// Whether a chain Windows builds from `leaf`, with `others`' certificates too, or none for null,
/// meets its SSL policy for a TLS server, or a client, as Schannel's would, fetching a root from
/// the list Windows trusts that the ROOT store hasn't got yet. The chain's name is mbedTLS's to
/// check.
bool meets_ssl_policy(PCCERT_CONTEXT leaf, HCERTSTORE others, const bool clients) {
    // wincrypt.h's usages are char*s, which it only reads.
    char* usage =
        const_cast<char*>(clients ? szOID_PKIX_KP_CLIENT_AUTH : szOID_PKIX_KP_SERVER_AUTH);
    CERT_CHAIN_PARA wanted{};
    wanted.cbSize = sizeof(wanted);
    wanted.RequestedUsage.dwType = USAGE_MATCH_TYPE_AND;
    wanted.RequestedUsage.Usage.cUsageIdentifier = 1;
    wanted.RequestedUsage.Usage.rgpszUsageIdentifier = &usage;
    PCCERT_CHAIN_CONTEXT built = nullptr;
    if (CertGetCertificateChain(nullptr, leaf, nullptr, others, &wanted, 0, nullptr, &built) ==
        FALSE) {
        return false;
    }

    // With no server name, the policy leaves the name alone.
    SSL_EXTRA_CERT_CHAIN_POLICY_PARA ssl{};
    ssl.cbSize = sizeof(ssl);
    ssl.dwAuthType = clients ? AUTHTYPE_CLIENT : AUTHTYPE_SERVER;
    CERT_CHAIN_POLICY_PARA policy{};
    policy.cbSize = sizeof(policy);
    policy.pvExtraPolicyPara = &ssl;
    CERT_CHAIN_POLICY_STATUS status{};
    status.cbSize = sizeof(status);
    const bool met =
        CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, built, &policy, &status) != FALSE &&
        status.dwError == 0;
    CertFreeCertificateChain(built);
    return met;
}

/// The ROOT store's roots that are valid now, for servers, and that Windows' SSL policy trusts
/// for servers, so none that Windows distrusts, as one it distrusts for servers from a date, or
/// whose properties leave server authentication out, which mbedTLS would otherwise trust where
/// Schannel wouldn't.
std::string read_roots() {
    std::string pem;
    HCERTSTORE store = CertOpenSystemStoreW(0, L"ROOT");
    if (store == nullptr) {
        return pem;
    }

    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    PCCERT_CONTEXT certificate = nullptr;
    while ((certificate = CertEnumCertificatesInStore(store, certificate)) != nullptr) {
        const CERT_INFO& info = *certificate->pCertInfo;
        if (certificate->dwCertEncodingType != X509_ASN_ENCODING ||
            CompareFileTime(&now, &info.NotBefore) < 0 ||
            CompareFileTime(&now, &info.NotAfter) > 0 || !for_servers(certificate) ||
            !meets_ssl_policy(certificate, nullptr, false)) {
            continue;
        }

        fb::runtime::append_pem(pem,
                                std::span(certificate->pbCertEncoded, certificate->cbCertEncoded));
    }

    CertCloseStore(store, 0);
    return pem;
}

/// Whether Windows trusts `chain`, a certificate and the others its side sent after it, for a
/// TLS server, or a client, as meets_ssl_policy has it.
bool os_trusts(const mbedtls_x509_crt& chain, const bool clients) {
    HCERTSTORE sent = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, nullptr);
    if (sent == nullptr) {
        return false;
    }

    PCCERT_CONTEXT leaf = nullptr;
    for (const mbedtls_x509_crt* at = &chain; at != nullptr && at->raw.p != nullptr;
         at = at->next) {
        (void)CertAddEncodedCertificateToStore(
            sent, X509_ASN_ENCODING, at->raw.p, static_cast<DWORD>(at->raw.len),
            CERT_STORE_ADD_ALWAYS, at == &chain ? &leaf : nullptr);
    }

    const bool trusted = leaf != nullptr && meets_ssl_policy(leaf, sent, clients);
    if (leaf != nullptr) {
        CertFreeCertificateContext(leaf);
    }

    CertCloseStore(sent, 0);
    return trusted;
}

/// What os_verify found of the chain it's checking so far, which mbedTLS gives it a certificate
/// at a time on one thread, from the root's end to depth 0.
struct checking_t {
    /// Whether a certificate's flaw was that no root it trusts signed it.
    bool untrusted = false;
    /// Whether one had a flaw besides.
    bool flawed = false;
};

thread_local checking_t checking;

#elifdef __APPLE__

std::string read_roots() {
    std::string pem;
    CFArrayRef anchors = nullptr;
    if (SecTrustCopyAnchorCertificates(&anchors) != errSecSuccess || anchors == nullptr) {
        return pem;
    }

    const CFIndex count = CFArrayGetCount(anchors);
    for (CFIndex i = 0; i < count; ++i) {
        const auto certificate =
            static_cast<SecCertificateRef>(const_cast<void*>(CFArrayGetValueAtIndex(anchors, i)));
        CFDataRef der = SecCertificateCopyData(certificate);
        if (der == nullptr) {
            continue;
        }

        fb::runtime::append_pem(
            pem, std::span(CFDataGetBytePtr(der), static_cast<std::size_t>(CFDataGetLength(der))));
        CFRelease(der);
    }

    CFRelease(anchors);
    return pem;
}

#else

/// Where distributions keep their bundles of roots, as Go looks for them.
constexpr std::array bundles{
    "/etc/ssl/certs/ca-certificates.crt",                // Debian, Ubuntu, Arch, Gentoo
    "/etc/pki/tls/certs/ca-bundle.crt",                  // Fedora, RHEL 6
    "/etc/ssl/ca-bundle.pem",                            // openSUSE
    "/etc/pki/tls/cacert.pem",                           // OpenELEC
    "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", // CentOS, RHEL 7
    "/etc/ssl/cert.pem",                                 // Alpine, the BSDs
    "/usr/local/share/certs/ca-root-nss.crt",            // FreeBSD
};

std::string read_roots() {
    // SSL_CERT_FILE names another bundle, as OpenSSL and Go take it.
    const char* given = std::getenv("SSL_CERT_FILE");
    if (given != nullptr && *given != '\0') {
        std::ifstream file(given, std::ios::binary);
        return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
    }

    for (const char* path : bundles) {
        std::ifstream file(path, std::ios::binary);
        if (file) {
            return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
        }
    }

    return {};
}

#endif

} // namespace

namespace fb::runtime {

const std::string& os_roots() {
    static std::once_flag read;
    static std::string roots;
    std::call_once(read, [] { roots = read_roots(); });
    return roots;
}

// Its pointers aren't const, as mbedTLS has them.
// NOLINTNEXTLINE(misc-const-correctness)
int os_verify(void* clients, mbedtls_x509_crt* certificate, const int depth, std::uint32_t* flags) {
#ifdef _WIN32
    // mbedTLS's flags are ints. A flag a callback leaves on any certificate fails the chain, so
    // a root none trusted is let go of until the chain's own certificate, at depth 0, and set
    // again there unless Windows trusts the chain.
    const auto not_trusted = static_cast<std::uint32_t>(MBEDTLS_X509_BADCERT_NOT_TRUSTED);
    if ((*flags & not_trusted) != 0) {
        checking.untrusted = true;
        *flags &= ~not_trusted;
    }

    checking.flawed = checking.flawed || *flags != 0;
    if (depth == 0) {
        const checking_t checked = std::exchange(checking, {});
        if (checked.untrusted && (checked.flawed || !os_trusts(*certificate, clients != nullptr))) {
            *flags |= not_trusted;
        }
    }
#else
    (void)clients;
    (void)certificate;
    (void)depth;
    (void)flags;
#endif
    return 0;
}

void append_pem(std::string& pem, const std::span<const std::uint8_t> der) {
    static constexpr std::string_view digits =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    pem += "-----BEGIN CERTIFICATE-----\n";
    // Base64, in lines of 64 characters: each 3 bytes are 4 characters, the last padded with '='.
    std::size_t line = 0;
    for (std::size_t at = 0; at < der.size(); at += 3) {
        const std::size_t left = der.size() - at;
        std::uint32_t group = static_cast<std::uint32_t>(der[at]) << 16U;
        if (left > 1) {
            group |= static_cast<std::uint32_t>(der[at + 1]) << 8U;
        }

        if (left > 2) {
            group |= der[at + 2];
        }

        pem += digits[(group >> 18U) & 63U];
        pem += digits[(group >> 12U) & 63U];
        pem += left > 1 ? digits[(group >> 6U) & 63U] : '=';
        pem += left > 2 ? digits[group & 63U] : '=';
        line += 4;
        if (line == 64) {
            pem += '\n';
            line = 0;
        }
    }

    if (line != 0) {
        pem += '\n';
    }

    pem += "-----END CERTIFICATE-----\n";
}

} // namespace fb::runtime
