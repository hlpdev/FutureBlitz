#include "tls_roots.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>
#include <string>
#include <string_view>
#include <vector>

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
