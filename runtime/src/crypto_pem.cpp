// std.crypto's PEM keys and X.509 certificates. mbedTLS decodes PEM's base64, its pk module reads
// and writes ECDSA P-256 keys, and its RSA module checks RSA keys of any size its bignums hold;
// this file reads the DER around them itself: PKCS #8 and SubjectPublicKeyInfo, which say a key's
// kind, the Ed25519 and X25519 keys in them, which mbedTLS doesn't know or changes, and
// certificates, which mbedTLS's X.509 doesn't read when their key is Ed25519, and whose names it
// writes as escapes rather than text.

#include "crypto_internal.hpp"
#include "fb/crypto.hpp"
#include "fb/runtime.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <mbedtls/pem.h>
#include <mbedtls/pk.h>
#include <optional>
#include <psa/crypto.h>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// mbedTLS's RSA module, which reads RSA keys as PKCS #1's DER, and has no C++ guard.
extern "C" {
#include <rsa_internal.h>
}

using fb::crypto::bytes_of;
using fb::crypto::curve_key_size;
using fb::crypto::exported;
using fb::crypto::exported_public;
using fb::crypto::KEY_ECDSA_P256;
using fb::crypto::KEY_ED25519;
using fb::crypto::KEY_RSA;
using fb::crypto::KEY_X25519;
using fb::crypto::p256_pair;
using fb::crypto::p256_public;
using fb::crypto::put_length;
using fb::crypto::set;
using fb::crypto::start;
using fb::crypto::stored_key_t;
using fb::crypto::string_of;

namespace {

using bytes_t = std::span<const std::uint8_t>;

/// What fb_crypto_key_from_pem gives for a text with no key, one std.crypto doesn't take, or one
/// that doesn't read.
constexpr std::int32_t no_key = -1;
constexpr std::int32_t unknown_key = -2;
constexpr std::int32_t bad_key = -3;

/// The most bits an RSA modulus has that std.crypto takes, which mbedTLS's bignums hold.
constexpr std::size_t rsa_max_bits = 8192;

/// DER's tags, of what std.crypto reads.
enum tag_t : std::uint8_t {
    INTEGER = 0x02,
    BIT_STRING = 0x03,
    OCTET_STRING = 0x04,
    NULL_VALUE = 0x05,
    OBJECT_ID = 0x06,
    UTF8_STRING = 0x0c,
    NUMERIC_STRING = 0x12,
    PRINTABLE_STRING = 0x13,
    T61_STRING = 0x14,
    IA5_STRING = 0x16,
    UTC_TIME = 0x17,
    GENERALIZED_TIME = 0x18,
    VISIBLE_STRING = 0x1a,
    UNIVERSAL_STRING = 0x1c,
    BMP_STRING = 0x1e,
    SEQUENCE = 0x30,
    SET = 0x31,
    // A certificate's [0] version, and PKCS #8's [0] attributes and [1] public key.
    FIRST = 0xa0,
    SECOND = 0x81,
};

// The algorithms' OBJECT IDENTIFIERs: rsaEncryption, id-ecPublicKey, Ed25519, and X25519.
template <std::size_t Size>
using oid_t = std::array<std::uint8_t, Size>;
constexpr oid_t<9> rsa_oid{ 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01 };
constexpr oid_t<7> ec_oid{ 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01 };
constexpr oid_t<3> ed25519_oid{ 0x2b, 0x65, 0x70 };
constexpr oid_t<3> x25519_oid{ 0x2b, 0x65, 0x6e };

/// DER's elements, taken one at a time from the front of `rest`.
struct der_t {
    bytes_t rest;

    /// The next element's tag and contents, which it takes, or nothing when it doesn't read.
    std::optional<std::pair<std::uint8_t, bytes_t>> take_any() {
        if (rest.size() < 2) {
            return std::nullopt;
        }

        std::size_t length = rest[1];
        std::size_t header = 2;
        if (length >= 0x80) {
            const std::size_t count = length & 0x7fU;
            if (count == 0 || count > 4 || rest.size() < header + count) {
                return std::nullopt;
            }

            length = 0;
            for (std::size_t i = 0; i < count; ++i) {
                length = (length << 8U) | rest[header + i];
            }
            header += count;
        }
        if (rest.size() - header < length) {
            return std::nullopt;
        }

        const std::uint8_t tag = rest[0];
        const bytes_t contents = rest.subspan(header, length);
        rest = rest.subspan(header + length);
        return std::pair{ tag, contents };
    }

    /// The contents of the next element when its tag is `tag`, which it takes, or nothing.
    std::optional<bytes_t> take(const std::uint8_t tag) {
        if (rest.empty() || rest[0] != tag) {
            return std::nullopt;
        }
        return take_any().transform([](const auto& element) { return element.second; });
    }

    /// The whole of the next element, its tag and length too, when its tag is `tag`.
    std::optional<bytes_t> take_whole(const std::uint8_t tag) {
        const bytes_t before = rest;
        return take(tag).transform(
            [&](bytes_t) { return before.first(before.size() - rest.size()); });
    }

    /// Takes the next element when its tag is `tag`, and tells whether there was one.
    bool skip(const std::uint8_t tag) {
        return take(tag).has_value();
    }
};

/// `contents` as one DER element of `tag`.
std::vector<std::uint8_t> element(const std::uint8_t tag, const bytes_t contents) {
    std::vector<std::uint8_t> der{ tag };
    put_length(der, contents.size());
    der.insert(der.end(), contents.begin(), contents.end());
    return der;
}

/// A key read: its kind times 2, plus 1 when it's private, and its bytes; or no_key,
/// unknown_key, or bad_key.
struct found_key_t {
    std::int32_t found = no_key;
    std::vector<std::uint8_t> bytes;
};

found_key_t key_of(const std::int32_t kind, const bool is_private, const bytes_t bytes) {
    return { .found = (kind * 2) + (is_private ? 1 : 0), .bytes = { bytes.begin(), bytes.end() } };
}

/// An RSA key of PKCS #1's DER, which mbedTLS's RSA module checks, with a modulus of up to 8192
/// bits, of any number of them. PSA would take none over 4096, or of an odd number.
found_key_t rsa_key(const bytes_t der, const bool is_private) {
    der_t outer{ der };
    der_t integers{ outer.take(SEQUENCE).value_or(bytes_t()) };
    if (is_private && !integers.skip(INTEGER)) {
        return { .found = bad_key };
    }

    // A modulus has a 0 first when its top bit is set, which isn't one of its bits.
    bytes_t modulus = integers.take(INTEGER).value_or(bytes_t());
    while (!modulus.empty() && modulus.front() == 0) {
        modulus = modulus.subspan(1);
    }
    if (!modulus.empty() &&
        ((modulus.size() - 1) * 8) + static_cast<std::size_t>(std::bit_width(modulus.front())) >
            rsa_max_bits) {
        return { .found = unknown_key };
    }

    mbedtls_rsa_context rsa;
    mbedtls_rsa_init(&rsa);
    const int read = is_private ? mbedtls_rsa_parse_key(&rsa, der.data(), der.size())
                                : mbedtls_rsa_parse_pubkey(&rsa, der.data(), der.size());
    mbedtls_rsa_free(&rsa);
    return read == 0 ? key_of(KEY_RSA, is_private, der) : found_key_t{ .found = bad_key };
}

/// A key mbedTLS's pk module read: a P-256 private key as its 32 bytes, and a public one as its
/// point, uncompressed, 65 bytes. Other curves are kinds std.crypto doesn't take.
found_key_t pk_key(const mbedtls_pk_context& pk) {
    const psa_key_type_t type = mbedtls_pk_get_key_type(&pk);
    if (!PSA_KEY_TYPE_IS_ECC(type) || PSA_KEY_TYPE_ECC_GET_FAMILY(type) != PSA_ECC_FAMILY_SECP_R1 ||
        mbedtls_pk_get_bitlen(&pk) != 256) {
        return { .found = unknown_key };
    }

    // Attributes for signing or verifying let the key be exported too.
    const bool is_private = PSA_KEY_TYPE_IS_KEY_PAIR(type);
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    if (mbedtls_pk_get_psa_attributes(
            &pk, is_private ? PSA_KEY_USAGE_SIGN_MESSAGE : PSA_KEY_USAGE_VERIFY_MESSAGE,
            &attributes) != 0) {
        return { .found = bad_key };
    }

    mbedtls_svc_key_id_t id = MBEDTLS_SVC_KEY_ID_INIT;
    const int imported = mbedtls_pk_import_into_psa(&pk, &attributes, &id);
    psa_reset_key_attributes(&attributes);
    if (imported != 0) {
        return { .found = bad_key };
    }

    const std::vector<std::uint8_t> bytes = is_private ? exported(id) : exported_public(id);
    psa_destroy_key(id);
    return bytes.empty() ? found_key_t{ .found = bad_key }
                         : key_of(KEY_ECDSA_P256, is_private, bytes);
}

/// What a pk module error says of the key: of a kind std.crypto doesn't take, or damaged.
std::int32_t pk_failure(const int error) {
    const bool unknown =
        error == MBEDTLS_ERR_PK_UNKNOWN_PK_ALG || error == MBEDTLS_ERR_PK_UNKNOWN_NAMED_CURVE ||
        error == MBEDTLS_ERR_PK_FEATURE_UNAVAILABLE || error == PSA_ERROR_NOT_SUPPORTED;
    return unknown ? unknown_key : bad_key;
}

/// An elliptic curve key, as SEC 1, PKCS #8, or SubjectPublicKeyInfo has it in `der`, which
/// mbedTLS's pk module reads.
found_key_t ec_key(const bytes_t der, const bool is_private) {
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    const int read = is_private ? mbedtls_pk_parse_key(&pk, der.data(), der.size(), nullptr, 0)
                                : mbedtls_pk_parse_public_key(&pk, der.data(), der.size());
    const found_key_t found = read == 0 ? pk_key(pk) : found_key_t{ .found = pk_failure(read) };
    mbedtls_pk_free(&pk);
    return found;
}

/// The key a SubjectPublicKeyInfo is, the whole of whose DER is `der`.
found_key_t public_key_of(const bytes_t der) {
    der_t outer{ der };
    der_t info{ outer.take(SEQUENCE).value_or(bytes_t()) };
    der_t algorithm{ info.take(SEQUENCE).value_or(bytes_t()) };
    const std::optional<bytes_t> key = info.take(BIT_STRING);
    const std::optional<bytes_t> oid = algorithm.take(OBJECT_ID);
    // The BIT STRING's first byte counts the bits after its last, which a key has none of.
    if (!outer.rest.empty() || !info.rest.empty() || !oid || !key || key->empty() ||
        key->front() != 0) {
        return { .found = bad_key };
    }

    if (std::ranges::equal(*oid, rsa_oid)) {
        algorithm.skip(NULL_VALUE);
        return algorithm.rest.empty() ? rsa_key(key->subspan(1), false)
                                      : found_key_t{ .found = bad_key };
    }
    if (std::ranges::equal(*oid, ed25519_oid) || std::ranges::equal(*oid, x25519_oid)) {
        if (!algorithm.rest.empty() || key->size() != curve_key_size + 1) {
            return { .found = bad_key };
        }
        return key_of(std::ranges::equal(*oid, ed25519_oid) ? KEY_ED25519 : KEY_X25519, false,
                      key->subspan(1));
    }
    if (std::ranges::equal(*oid, ec_oid)) {
        return ec_key(der, false);
    }

    return { .found = unknown_key };
}

/// The key a PKCS #8 private key is, of either version: RFC 5958's second can have attributes
/// and the public key after the private one.
found_key_t private_key_of(const bytes_t der) {
    der_t outer{ der };
    der_t info{ outer.take(SEQUENCE).value_or(bytes_t()) };
    const std::optional<bytes_t> version = info.take(INTEGER);
    der_t algorithm{ info.take(SEQUENCE).value_or(bytes_t()) };
    const std::optional<bytes_t> key = info.take(OCTET_STRING);
    info.skip(FIRST);
    info.skip(SECOND);
    const std::optional<bytes_t> oid = algorithm.take(OBJECT_ID);
    if (!outer.rest.empty() || !info.rest.empty() || !version || version->size() != 1 ||
        version->front() > 1 || !oid || !key) {
        return { .found = bad_key };
    }

    if (std::ranges::equal(*oid, rsa_oid)) {
        algorithm.skip(NULL_VALUE);
        return algorithm.rest.empty() ? rsa_key(*key, true) : found_key_t{ .found = bad_key };
    }
    if (std::ranges::equal(*oid, ed25519_oid) || std::ranges::equal(*oid, x25519_oid)) {
        // The key is an OCTET STRING of its 32 bytes, as they are: mbedTLS's pk would change an
        // X25519 one's to clamp it.
        der_t inner{ *key };
        const std::optional<bytes_t> bytes = inner.take(OCTET_STRING);
        if (!algorithm.rest.empty() || !inner.rest.empty() || !bytes ||
            bytes->size() != curve_key_size) {
            return { .found = bad_key };
        }
        return key_of(std::ranges::equal(*oid, ed25519_oid) ? KEY_ED25519 : KEY_X25519, true,
                      *bytes);
    }
    if (std::ranges::equal(*oid, ec_oid)) {
        return ec_key(der, true);
    }

    return { .found = unknown_key };
}

/// The key a PEM block of `label` is, whose DER is `der`.
found_key_t key_in_block(const std::string_view label, const bytes_t der) {
    if (label == "PRIVATE KEY") {
        return private_key_of(der);
    }
    if (label == "PUBLIC KEY") {
        return public_key_of(der);
    }
    if (label == "RSA PRIVATE KEY" || label == "RSA PUBLIC KEY") {
        return rsa_key(der, label == "RSA PRIVATE KEY");
    }
    if (label == "EC PRIVATE KEY") {
        return ec_key(der, true);
    }

    // An ENCRYPTED PRIVATE KEY, or one of a kind std.crypto doesn't take, like DSA's.
    return { .found = unknown_key };
}

/// The PEM block in `text` that starts at `at`, of `label`: its DER and how many bytes of the
/// text it takes, or mbedTLS's error, which is MBEDTLS_ERR_PEM_PASSWORD_REQUIRED for an
/// encrypted one.
struct block_t {
    int error = 0;
    std::vector<std::uint8_t> der;
    std::size_t used = 0;
};

block_t pem_block(const std::string& text, const std::size_t at, const std::string_view label) {
    mbedtls_pem_context pem;
    mbedtls_pem_init(&pem);
    const std::string header = "-----BEGIN " + std::string(label) + "-----";
    const std::string footer = "-----END " + std::string(label) + "-----";
    block_t block;
    block.error = mbedtls_pem_read_buffer(&pem, header.c_str(), footer.c_str(),
                                          reinterpret_cast<const unsigned char*>(text.c_str() + at),
                                          nullptr, 0, &block.used);
    if (block.error == 0) {
        std::size_t size = 0;
        const unsigned char* data = mbedtls_pem_get_buffer(&pem, &size);
        block.der.assign(data, data + size);
    }

    mbedtls_pem_free(&pem);
    return block;
}

/// `der` as PEM, under `label`.
fb_string_t* pem_of(const bytes_t der, const std::string& label) {
    const std::string header = "-----BEGIN " + label + "-----\n";
    const std::string footer = "-----END " + label + "-----\n";
    std::vector<unsigned char> text((der.size() * 2) + header.size() + footer.size() + 64);
    std::size_t size = 0;
    if (mbedtls_pem_write_buffer(header.c_str(), footer.c_str(), der.data(), der.size(),
                                 text.data(), text.size(), &size) != 0) {
        return nullptr;
    }

    // The size counts the NUL at the end.
    return fb_string_from_bytes(reinterpret_cast<const char*>(text.data()),
                                static_cast<std::int64_t>(size - 1));
}

/// What `oid` names an attribute of a Name, as OpenSSL and Python's ssl write it, or its dotted
/// numbers when it's another.
std::string attribute_name(const bytes_t oid) {
    struct named_t {
        std::string_view oid;
        std::string_view name;
    };
    // The OIDs' bytes, as text: X.520's 2.5.4 ones, PKCS #9's email address, and RFC 4519's
    // domain component and user ID.
    static constexpr std::array<named_t, 19> names{
        {
            { .oid = "\x55\x04\x03", .name = "CN" },
            { .oid = "\x55\x04\x04", .name = "SN" },
            { .oid = "\x55\x04\x05", .name = "serialNumber" },
            { .oid = "\x55\x04\x06", .name = "C" },
            { .oid = "\x55\x04\x07", .name = "L" },
            { .oid = "\x55\x04\x08", .name = "ST" },
            { .oid = "\x55\x04\x09", .name = "street" },
            { .oid = "\x55\x04\x0a", .name = "O" },
            { .oid = "\x55\x04\x0b", .name = "OU" },
            { .oid = "\x55\x04\x0c", .name = "title" },
            { .oid = "\x55\x04\x11", .name = "postalCode" },
            { .oid = "\x55\x04\x2a", .name = "GN" },
            { .oid = "\x55\x04\x2b", .name = "initials" },
            { .oid = "\x55\x04\x2c", .name = "generationQualifier" },
            { .oid = "\x55\x04\x2e", .name = "dnQualifier" },
            { .oid = "\x55\x04\x41", .name = "pseudonym" },
            { .oid = "\x2a\x86\x48\x86\xf7\x0d\x01\x09\x01", .name = "emailAddress" },
            { .oid = "\x09\x92\x26\x89\x93\xf2\x2c\x64\x01\x19", .name = "DC" },
            { .oid = "\x09\x92\x26\x89\x93\xf2\x2c\x64\x01\x01", .name = "UID" },
        },
    };
    const std::string_view bytes(reinterpret_cast<const char*>(oid.data()), oid.size());
    for (const named_t& named : names) {
        if (named.oid == bytes) {
            return std::string(named.name);
        }
    }

    // The first byte is two numbers, 40 times the first and the second; the rest are base 128,
    // with the top bit set on each byte but a number's last.
    std::string dotted;
    std::uint64_t number = 0;
    for (const std::uint8_t byte : oid) {
        number = (number << 7U) | (byte & 0x7fU);
        if ((byte & 0x80U) != 0) {
            continue;
        }
        if (dotted.empty()) {
            const std::uint64_t first = std::min<std::uint64_t>(number / 40, 2);
            dotted = std::to_string(first) + "." + std::to_string(number - (first * 40));
        } else {
            dotted += "." + std::to_string(number);
        }
        number = 0;
    }
    return dotted;
}

/// Appends `code`, a code point, to `text` as UTF-8, or U+FFFD for one that isn't one.
void put_code_point(std::string& text, std::uint32_t code) {
    if (code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) {
        code = 0xfffd;
    }

    if (code < 0x80) {
        text += static_cast<char>(code);
    } else if (code < 0x800) {
        text += static_cast<char>(0xc0U | (code >> 6U));
        text += static_cast<char>(0x80U | (code & 0x3fU));
    } else if (code < 0x10000) {
        text += static_cast<char>(0xe0U | (code >> 12U));
        text += static_cast<char>(0x80U | ((code >> 6U) & 0x3fU));
        text += static_cast<char>(0x80U | (code & 0x3fU));
    } else {
        text += static_cast<char>(0xf0U | (code >> 18U));
        text += static_cast<char>(0x80U | ((code >> 12U) & 0x3fU));
        text += static_cast<char>(0x80U | ((code >> 6U) & 0x3fU));
        text += static_cast<char>(0x80U | (code & 0x3fU));
    }
}

/// A string of a Name as UTF-8, by its tag: the ASCII ones and UTF8String as they are,
/// T61String as Latin-1, as OpenSSL reads it, BMPString as UTF-16 and UniversalString as UTF-32,
/// big-endian. Nothing for another tag, or one whose bytes don't fit it.
std::optional<std::string> string_text(const std::uint8_t tag, const bytes_t bytes) {
    std::string text;
    switch (tag) {
    case UTF8_STRING:
    case NUMERIC_STRING:
    case PRINTABLE_STRING:
    case IA5_STRING:
    case VISIBLE_STRING:
        return std::string(bytes.begin(), bytes.end());
    case T61_STRING:
        for (const std::uint8_t byte : bytes) {
            put_code_point(text, byte);
        }
        return text;
    case BMP_STRING:
        if (bytes.size() % 2 != 0) {
            return std::nullopt;
        }
        for (std::size_t i = 0; i < bytes.size(); i += 2) {
            std::uint32_t code = (static_cast<std::uint32_t>(bytes[i]) << 8U) | bytes[i + 1];
            // A high surrogate and a low one are one code point past U+FFFF.
            if (code >= 0xd800 && code < 0xdc00 && i + 3 < bytes.size()) {
                const std::uint32_t low =
                    (static_cast<std::uint32_t>(bytes[i + 2]) << 8U) | bytes[i + 3];
                if (low >= 0xdc00 && low < 0xe000) {
                    code = 0x10000 + ((code - 0xd800) << 10U) + (low - 0xdc00);
                    i += 2;
                }
            }
            put_code_point(text, code);
        }
        return text;
    case UNIVERSAL_STRING:
        if (bytes.size() % 4 != 0) {
            return std::nullopt;
        }
        for (std::size_t i = 0; i < bytes.size(); i += 4) {
            put_code_point(text, (static_cast<std::uint32_t>(bytes[i]) << 24U) |
                                     (static_cast<std::uint32_t>(bytes[i + 1]) << 16U) |
                                     (static_cast<std::uint32_t>(bytes[i + 2]) << 8U) |
                                     bytes[i + 3]);
        }
        return text;
    default:
        return std::nullopt;
    }
}

/// An attribute's value as RFC 4514 writes it: its text, with a backslash before what would read
/// as the Name's own, and before a space or # first, or a space last; or, when it isn't a string,
/// # and the hex of its DER.
std::string value_text(const std::uint8_t tag, const bytes_t contents, const bytes_t whole) {
    const std::optional<std::string> text = string_text(tag, contents);
    if (!text) {
        std::string hex = "#";
        for (const std::uint8_t byte : whole) {
            std::array<char, 3> digits{};
            std::snprintf(digits.data(), digits.size(), "%02X", byte);
            hex += digits.data();
        }
        return hex;
    }

    const std::string& value = *text;
    std::string escaped;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        const bool special = std::string_view(",+\"\\<>;").contains(c);
        const bool first = i == 0 && (c == ' ' || c == '#');
        const bool last = i + 1 == value.size() && c == ' ';
        if (c == '\0') {
            escaped += "\\00";
            continue;
        }
        if (special || first || last) {
            escaped += '\\';
        }
        escaped += c;
    }
    return escaped;
}

/// A Name as text, its attributes in the certificate's order, as OpenSSL and Python's ssl show
/// it: "C=NL, O=Example, CN=example.com", with " + " between the values of one part.
std::optional<std::string> name_of(const bytes_t name) {
    der_t parts{ name };
    std::string text;
    while (!parts.rest.empty()) {
        const std::optional<bytes_t> part = parts.take(SET);
        if (!part) {
            return std::nullopt;
        }

        der_t attributes{ *part };
        for (bool first = true; !attributes.rest.empty(); first = false) {
            der_t attribute{ attributes.take(SEQUENCE).value_or(bytes_t()) };
            const std::optional<bytes_t> oid = attribute.take(OBJECT_ID);
            const bytes_t before = attribute.rest;
            const auto value = attribute.take_any();
            if (!oid || !value || !attribute.rest.empty()) {
                return std::nullopt;
            }

            if (!text.empty()) {
                text += first ? ", " : " + ";
            }
            text += attribute_name(*oid) + "=" + value_text(value->first, value->second, before);
        }
    }
    return text;
}

/// A UTCTime or GeneralizedTime, as RFC 5280 has them, in UTC to the second, as
/// "2026-10-23T12:00:00Z", or nothing when it isn't one.
std::optional<std::string> time_of(const std::uint8_t tag, const bytes_t bytes) {
    const std::size_t digits = tag == UTC_TIME ? 12 : 14;
    if ((tag != UTC_TIME && tag != GENERALIZED_TIME) || bytes.size() != digits + 1 ||
        bytes.back() != 'Z' ||
        !std::all_of(bytes.begin(), bytes.end() - 1,
                     [](const std::uint8_t c) { return c >= '0' && c <= '9'; })) {
        return std::nullopt;
    }

    const auto number = [&](const std::size_t at) {
        return ((bytes[at] - '0') * 10) + (bytes[at + 1] - '0');
    };
    std::size_t at = 0;
    int year = number(0);
    if (tag == UTC_TIME) {
        // RFC 5280: a UTCTime's years from 50 are the 1900s, and the rest the 2000s.
        year += year >= 50 ? 1900 : 2000;
        at = 2;
    } else {
        year = (year * 100) + number(2);
        at = 4;
    }

    const int month = number(at);
    const int day = number(at + 2);
    const int hour = number(at + 4);
    const int minute = number(at + 6);
    const int second = number(at + 8);
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 59) {
        return std::nullopt;
    }

    std::array<char, 32> text{};
    std::snprintf(text.data(), text.size(), "%04d-%02d-%02dT%02d:%02d:%02dZ", year, month, day,
                  hour, minute, second);
    return std::string(text.data());
}

/// What a certificate says, read from its DER.
struct certificate_t {
    std::vector<std::uint8_t> der;
    std::string subject;
    std::string issuer;
    std::string serial;
    std::string not_before;
    std::string not_after;
    found_key_t key;
};

/// The certificate whose DER is `der`, or nothing when it doesn't read.
std::optional<certificate_t> certificate_of(const bytes_t der) {
    der_t outer{ der };
    der_t parts{ outer.take(SEQUENCE).value_or(bytes_t()) };
    const std::optional<bytes_t> signed_part = parts.take(SEQUENCE);
    if (!outer.rest.empty() || !signed_part || !parts.skip(SEQUENCE) || !parts.skip(BIT_STRING) ||
        !parts.rest.empty()) {
        return std::nullopt;
    }

    // Its version, from 0 for X.509's first to 2 for its third, the serial number, the algorithm
    // it's signed by, the issuer, when it's valid, the subject, and the public key; and then what
    // later versions add.
    der_t fields{ *signed_part };
    fields.skip(FIRST);
    const std::optional<bytes_t> serial = fields.take(INTEGER);
    const bool signed_by = fields.skip(SEQUENCE);
    const std::optional<bytes_t> issuer = fields.take(SEQUENCE);
    der_t validity{ fields.take(SEQUENCE).value_or(bytes_t()) };
    const std::optional<bytes_t> subject = fields.take(SEQUENCE);
    const std::optional<bytes_t> key = fields.take_whole(SEQUENCE);
    const auto from = validity.take_any();
    const auto to = validity.take_any();
    if (!serial || serial->empty() || !signed_by || !issuer || !subject || !key || !from || !to ||
        !validity.rest.empty()) {
        return std::nullopt;
    }

    certificate_t certificate;
    certificate.der.assign(der.begin(), der.end());
    const std::optional<std::string> subject_text = name_of(*subject);
    const std::optional<std::string> issuer_text = name_of(*issuer);
    const std::optional<std::string> not_before = time_of(from->first, from->second);
    const std::optional<std::string> not_after = time_of(to->first, to->second);
    if (!subject_text || !issuer_text || !not_before || !not_after) {
        return std::nullopt;
    }

    certificate.subject = *subject_text;
    certificate.issuer = *issuer_text;
    certificate.not_before = *not_before;
    certificate.not_after = *not_after;
    certificate.key = public_key_of(*key);

    // DER puts a 0 before a serial whose top bit is set, which isn't the number's.
    bytes_t number = *serial;
    while (number.size() > 1 && number.front() == 0) {
        number = number.subspan(1);
    }
    for (std::size_t i = 0; i < number.size(); ++i) {
        std::array<char, 4> digits{};
        std::snprintf(digits.data(), digits.size(), i == 0 ? "%02X" : ":%02X", number[i]);
        certificate.serial += digits.data();
    }
    return certificate;
}

const certificate_t& certificate(const void* certificates, const std::int64_t index) {
    return static_cast<const std::vector<certificate_t>*>(certificates)
        ->at(static_cast<std::size_t>(index));
}

} // namespace

extern "C" {

std::int32_t fb_crypto_key_from_pem(const fb_string_t* pem, fb_array_t* output) {
    start();
    // The first block whose label ends in " KEY", which mbedTLS reads from a C string.
    const std::span<const std::uint8_t> bytes = bytes_of(pem);
    const std::string text(bytes.begin(), bytes.end());
    constexpr std::string_view begin = "-----BEGIN ";
    for (std::size_t at = text.find(begin); at != std::string::npos;
         at = text.find(begin, at + 1)) {
        const std::size_t start = at + begin.size();
        const std::size_t end = text.find("-----", start);
        const std::string_view label = std::string_view(text).substr(start, end - start);
        if (end == std::string::npos || !label.ends_with(" KEY")) {
            continue;
        }

        const block_t block = pem_block(text, at, label);
        if (block.error == MBEDTLS_ERR_PEM_PASSWORD_REQUIRED ||
            block.error == MBEDTLS_ERR_PEM_FEATURE_UNAVAILABLE) {
            return unknown_key;
        }
        if (block.error != 0) {
            return bad_key;
        }

        const found_key_t found = key_in_block(label, block.der);
        if (found.found >= 0) {
            set(output, found.bytes);
        }
        return found.found;
    }

    return no_key;
}

fb_string_t* fb_crypto_key_to_pem(const std::int32_t kind, const std::int32_t is_private,
                                  const fb_array_t* key) {
    const std::span<const std::uint8_t> bytes = bytes_of(key);
    if (kind == KEY_ED25519 || kind == KEY_X25519) {
        if (bytes.size() != curve_key_size) {
            return nullptr;
        }

        // PKCS #8, its key an OCTET STRING of its 32 bytes; or SubjectPublicKeyInfo.
        const std::array<std::uint8_t, 1> version{};
        const std::vector<std::uint8_t> oid =
            element(OBJECT_ID, kind == KEY_ED25519 ? bytes_t(ed25519_oid) : bytes_t(x25519_oid));
        std::vector<std::uint8_t> parts;
        if (is_private != 0) {
            parts = element(INTEGER, version);
            std::ranges::copy(element(SEQUENCE, oid), std::back_inserter(parts));
            std::ranges::copy(element(OCTET_STRING, element(OCTET_STRING, bytes)),
                              std::back_inserter(parts));
        } else {
            std::vector<std::uint8_t> bits{ 0 };
            bits.insert(bits.end(), bytes.begin(), bytes.end());
            parts = element(SEQUENCE, oid);
            std::ranges::copy(element(BIT_STRING, bits), std::back_inserter(parts));
        }
        return pem_of(element(SEQUENCE, parts), is_private != 0 ? "PRIVATE KEY" : "PUBLIC KEY");
    }

    if (kind == KEY_RSA) {
        // A private key in PKCS #1, and a public one in SubjectPublicKeyInfo, as OpenSSL writes
        // them.
        if (rsa_key(bytes, is_private != 0).found < 0) {
            return nullptr;
        }
        if (is_private != 0) {
            return pem_of(bytes, "RSA PRIVATE KEY");
        }

        std::vector<std::uint8_t> algorithm = element(OBJECT_ID, rsa_oid);
        algorithm.insert(algorithm.end(), { NULL_VALUE, 0 });
        std::vector<std::uint8_t> bits{ 0 };
        bits.insert(bits.end(), bytes.begin(), bytes.end());
        std::vector<std::uint8_t> parts = element(SEQUENCE, algorithm);
        std::ranges::copy(element(BIT_STRING, bits), std::back_inserter(parts));
        return pem_of(element(SEQUENCE, parts), "PUBLIC KEY");
    }

    // A P-256 private key in SEC 1, and a public one in SubjectPublicKeyInfo.
    const stored_key_t made(is_private != 0 ? p256_pair : p256_public, PSA_KEY_USAGE_EXPORT,
                            PSA_ALG_NONE, bytes);
    if (!made.made()) {
        return nullptr;
    }

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    std::vector<unsigned char> text(4096);
    const int copied = is_private != 0 ? mbedtls_pk_copy_from_psa(made.id(), &pk)
                                       : mbedtls_pk_copy_public_from_psa(made.id(), &pk);
    int written = copied;
    if (copied == 0) {
        written = is_private != 0 ? mbedtls_pk_write_key_pem(&pk, text.data(), text.size())
                                  : mbedtls_pk_write_pubkey_pem(&pk, text.data(), text.size());
    }

    mbedtls_pk_free(&pk);
    if (written != 0) {
        return nullptr;
    }

    return fb_string_from_cstring(reinterpret_cast<const char*>(text.data()));
}

void* fb_crypto_certificates_new(const fb_string_t* pem) {
    start();
    const std::span<const std::uint8_t> bytes = bytes_of(pem);
    const std::string text(bytes.begin(), bytes.end());
    constexpr std::string_view header = "-----BEGIN CERTIFICATE-----";
    std::vector<certificate_t> certificates;
    for (std::size_t at = text.find(header); at != std::string::npos;
         at = text.find(header, at + 1)) {
        const block_t block = pem_block(text, at, "CERTIFICATE");
        std::optional<certificate_t> read =
            block.error == 0 ? certificate_of(block.der) : std::nullopt;
        if (!read) {
            return nullptr;
        }

        certificates.push_back(std::move(*read));
    }

    if (certificates.empty()) {
        return nullptr;
    }
    return new std::vector<certificate_t>(std::move(certificates));
}

std::int64_t fb_crypto_certificates_count(const void* certificates) {
    return static_cast<std::int64_t>(
        static_cast<const std::vector<certificate_t>*>(certificates)->size());
}

fb_string_t* fb_crypto_certificate_subject(const void* certificates, const std::int64_t index) {
    return string_of(certificate(certificates, index).subject);
}

fb_string_t* fb_crypto_certificate_issuer(const void* certificates, const std::int64_t index) {
    return string_of(certificate(certificates, index).issuer);
}

fb_string_t* fb_crypto_certificate_serial(const void* certificates, const std::int64_t index) {
    return string_of(certificate(certificates, index).serial);
}

fb_string_t* fb_crypto_certificate_not_before(const void* certificates, const std::int64_t index) {
    return string_of(certificate(certificates, index).not_before);
}

fb_string_t* fb_crypto_certificate_not_after(const void* certificates, const std::int64_t index) {
    return string_of(certificate(certificates, index).not_after);
}

void fb_crypto_certificate_der(const void* certificates, const std::int64_t index,
                               fb_array_t* output) {
    set(output, certificate(certificates, index).der);
}

std::int32_t fb_crypto_certificate_key(const void* certificates, const std::int64_t index,
                                       fb_array_t* output) {
    const found_key_t& key = certificate(certificates, index).key;
    if (key.found >= 0) {
        set(output, key.bytes);
    }
    return key.found;
}

void fb_crypto_certificates_free(void* certificates) {
    delete static_cast<std::vector<certificate_t>*>(certificates);
}
}
