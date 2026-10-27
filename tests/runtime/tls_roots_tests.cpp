#include "tls_roots.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <ios>
#include <iterator>
#include <mbedtls/x509.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// windows.h first, which wincrypt.h needs.
#include <wincrypt.h>
#endif

namespace {

/// The PEM block append_pem writes for `text`'s bytes.
std::string pem_of(const std::string_view text) {
    const std::vector<std::uint8_t> der(text.begin(), text.end());
    std::string pem;
    fb::runtime::append_pem(pem, der);
    return pem;
}

std::string block(const std::string_view lines) {
    return "-----BEGIN CERTIFICATE-----\n" + std::string(lines) + "-----END CERTIFICATE-----\n";
}

#ifdef _WIN32

/// Whether Windows' own SSL policy trusts `certificate` as a TLS server's whole chain, as
/// Schannel would, asked of CryptoAPI here, apart from os_verify.
bool windows_trusts(const mbedtls_x509_crt& certificate) {
    PCCERT_CONTEXT context = CertCreateCertificateContext(X509_ASN_ENCODING, certificate.raw.p,
                                                          static_cast<DWORD>(certificate.raw.len));
    REQUIRE(context != nullptr);
    // wincrypt.h's usages are char*s, which it only reads.
    char* usage = const_cast<char*>(szOID_PKIX_KP_SERVER_AUTH);
    CERT_CHAIN_PARA wanted{};
    wanted.cbSize = sizeof(wanted);
    wanted.RequestedUsage.dwType = USAGE_MATCH_TYPE_AND;
    wanted.RequestedUsage.Usage.cUsageIdentifier = 1;
    wanted.RequestedUsage.Usage.rgpszUsageIdentifier = &usage;
    PCCERT_CHAIN_CONTEXT built = nullptr;
    bool trusted = false;
    if (CertGetCertificateChain(nullptr, context, nullptr, nullptr, &wanted, 0, nullptr, &built) !=
        FALSE) {
        SSL_EXTRA_CERT_CHAIN_POLICY_PARA ssl{};
        ssl.cbSize = sizeof(ssl);
        ssl.dwAuthType = AUTHTYPE_SERVER;
        CERT_CHAIN_POLICY_PARA policy{};
        policy.cbSize = sizeof(policy);
        policy.pvExtraPolicyPara = &ssl;
        CERT_CHAIN_POLICY_STATUS status{};
        status.cbSize = sizeof(status);
        trusted = CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, built, &policy,
                                                   &status) != FALSE &&
                  status.dwError == 0;
        CertFreeCertificateChain(built);
    }

    CertFreeCertificateContext(context);
    return trusted;
}

#endif

} // namespace

TEST_CASE("append_pem writes bytes as base64 between PEM's lines", "[runtime][tls]") {
    // RFC 4648's test vectors.
    CHECK(pem_of("") == block(""));
    CHECK(pem_of("f") == block("Zg==\n"));
    CHECK(pem_of("fo") == block("Zm8=\n"));
    CHECK(pem_of("foo") == block("Zm9v\n"));
    CHECK(pem_of("foob") == block("Zm9vYg==\n"));
    CHECK(pem_of("fooba") == block("Zm9vYmE=\n"));
    CHECK(pem_of("foobar") == block("Zm9vYmFy\n"));
    // 48 bytes fill a line of 64 characters, and the 49th starts the next.
    const std::string full(48, 'a');
    const std::string line(64, 'Y');
    CHECK(pem_of(full).starts_with("-----BEGIN CERTIFICATE-----\nYWFh"));
    CHECK(pem_of(full).size() == block(line + "\n").size());
    CHECK(pem_of(full + "a").ends_with("\nYQ==\n-----END CERTIFICATE-----\n"));
    // Blocks one after another make a bundle.
    std::string bundle;
    fb::runtime::append_pem(bundle, std::vector<std::uint8_t>{ 'f' });
    fb::runtime::append_pem(bundle, std::vector<std::uint8_t>{ 'f', 'o' });
    CHECK(bundle == block("Zg==\n") + block("Zm8=\n"));
}

TEST_CASE("the OS's roots are certificates that mbedTLS reads", "[runtime][tls]") {
    // Each CI runner's OS has its roots: Windows' store, macOS's anchors, and Ubuntu's bundle.
    const std::string& roots = fb::runtime::os_roots();
    REQUIRE(roots.contains("-----BEGIN CERTIFICATE-----"));
    CHECK(&fb::runtime::os_roots() == &roots);
    REQUIRE(psa_crypto_init() == PSA_SUCCESS);
    mbedtls_x509_crt chain;
    mbedtls_x509_crt_init(&chain);
    // It gives how many it couldn't read, which an OS may have of a kind mbedTLS doesn't take.
    const int result = mbedtls_x509_crt_parse(
        &chain, reinterpret_cast<const unsigned char*>(roots.c_str()), roots.size() + 1);
    CHECK(result >= 0);
    std::size_t count = 0;
    for (const mbedtls_x509_crt* certificate = &chain;
         certificate != nullptr && certificate->raw.len != 0; certificate = certificate->next) {
        ++count;
    }

    CHECK(count > 0);
    mbedtls_x509_crt_free(&chain);
}

#ifdef _WIN32

TEST_CASE("Windows' roots are those its SSL policy trusts for servers", "[runtime][tls]") {
    // A root the store keeps that Windows distrusts for servers, as from a date, or by its
    // properties, is left out, so mbedTLS never trusts one Schannel wouldn't.
    const std::string& roots = fb::runtime::os_roots();
    REQUIRE(psa_crypto_init() == PSA_SUCCESS);
    mbedtls_x509_crt chain;
    mbedtls_x509_crt_init(&chain);
    REQUIRE(mbedtls_x509_crt_parse(&chain, reinterpret_cast<const unsigned char*>(roots.c_str()),
                                   roots.size() + 1) >= 0);
    std::size_t count = 0;
    for (const mbedtls_x509_crt* root = &chain; root != nullptr && root->raw.len != 0;
         root = root->next) {
        CHECK(windows_trusts(*root));
        ++count;
    }

    CHECK(count > 0);
    mbedtls_x509_crt_free(&chain);
}

#endif

TEST_CASE("a chain none of the OS's roots signed is trusted as the OS trusts it",
          "[runtime][tls]") {
    REQUIRE(psa_crypto_init() == PSA_SUCCESS);
    const auto not_trusted = static_cast<std::uint32_t>(MBEDTLS_X509_BADCERT_NOT_TRUSTED);
    const auto cn_mismatch = static_cast<std::uint32_t>(MBEDTLS_X509_BADCERT_CN_MISMATCH);
    // What mbedTLS finds of a certificate, as a server's chain, with no roots of its own, which
    // os_verify checks too when `os` is set.
    const auto check = [](const mbedtls_x509_crt& certificate, const char* name, const bool os) {
        mbedtls_x509_crt alone;
        mbedtls_x509_crt_init(&alone);
        REQUIRE(mbedtls_x509_crt_parse_der(&alone, certificate.raw.p, certificate.raw.len) == 0);
        std::uint32_t flags = 0;
        (void)mbedtls_x509_crt_verify(&alone, nullptr, nullptr, name, &flags,
                                      os ? fb::runtime::os_verify : nullptr, nullptr);
        mbedtls_x509_crt_free(&alone);
        return flags;
    };

    // A root no OS trusts, the HTTP test server's, which mbedTLS reads with the NUL after it.
    std::ifstream file(FB_TEST_TLS_CA, std::ios::binary);
    const std::string pem{ std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
    mbedtls_x509_crt test_ca;
    mbedtls_x509_crt_init(&test_ca);
    REQUIRE(mbedtls_x509_crt_parse(&test_ca, reinterpret_cast<const unsigned char*>(pem.c_str()),
                                   pem.size() + 1) == 0);
    CHECK(check(test_ca, nullptr, true) == not_trusted);
    mbedtls_x509_crt_free(&test_ca);

    // Each of the OS's own roots whose one flaw, to mbedTLS alone, is that none of its roots
    // signed it, as if the roots mbedTLS has didn't hold it, as Windows' ROOT store doesn't hold a
    // root Windows trusts until it fetches it; one of a hash mbedTLS refuses, like SHA-1, has a
    // flaw of its own. On Windows, os_verify trusts it just when Windows' own SSL policy does,
    // which a root the store keeps can still not be, as one distrusted for servers from a date;
    // elsewhere, the OS's roots are all mbedTLS's, so it changes nothing.
    const std::string& roots = fb::runtime::os_roots();
    mbedtls_x509_crt chain;
    mbedtls_x509_crt_init(&chain);
    REQUIRE(mbedtls_x509_crt_parse(&chain, reinterpret_cast<const unsigned char*>(roots.c_str()),
                                   roots.size() + 1) >= 0);
    std::size_t checked = 0;
    const mbedtls_x509_crt* trusted = nullptr;
    for (const mbedtls_x509_crt* root = &chain; root != nullptr && root->raw.len != 0;
         root = root->next) {
        if (check(*root, nullptr, false) != not_trusted) {
            continue;
        }

        ++checked;
#ifdef _WIN32
        const bool windows = windows_trusts(*root);
#else
        const bool windows = false;
#endif
        CHECK(check(*root, nullptr, true) == (windows ? 0 : not_trusted));
        if (windows && trusted == nullptr) {
            trusted = root;
        }
    }

    CHECK(checked > 0);
#ifdef _WIN32
    // A name it isn't for is a flaw the OS's check doesn't undo, and it isn't asked.
    if (trusted != nullptr) {
        CHECK(check(*trusted, "nothing.invalid", true) == (not_trusted | cn_mismatch));
    }
#endif
    mbedtls_x509_crt_free(&chain);
#ifdef _WIN32
    if (trusted == nullptr) {
        SKIP("none of this machine's roots is one Windows' SSL policy trusts");
    }
#endif
}
