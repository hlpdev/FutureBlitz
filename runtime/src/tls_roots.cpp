// The OS's trusted roots, for TLS: the certificates in Windows' ROOT store, macOS's system
// anchors, or a distribution's bundle file, as PEM, which mbedTLS reads.

#include "tls_roots.hpp"

#include <cstddef>
#include <cstdint>
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
            CompareFileTime(&now, &info.NotAfter) > 0 || !for_servers(certificate)) {
            continue;
        }

        fb::runtime::append_pem(pem,
                                std::span(certificate->pbCertEncoded, certificate->cbCertEncoded));
    }

    CertCloseStore(store, 0);
    return pem;
}

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
