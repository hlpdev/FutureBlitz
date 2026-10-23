# The review of M6 step 21 (std.crypto)

13 confirmed findings: 0 high, 6 medium, 7 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## S21-hash-1 (medium): Hmac panics with an internal PSA error for a key of 8192 bytes or more

Status: fixed in 955872f

Where: runtime/src/crypto.cpp:235

fb_crypto_hmac imports the key as-is with psa_import_key as a PSA_KEY_TYPE_HMAC key. PSA refuses any key over PSA_MAX_KEY_BITS (0xfff8 bits, 8191 bytes), so the import fails. check() then panics with "std.crypto's HMAC failed with PSA error -135", which has no place. derive.fb (line 19) says HMAC takes a key "of any length", and RFC 2104 allows one: a key longer than the hash's block is hashed first. std checks nothing here, and crypto.hpp says a bad size that reaches the runtime is a bug. Keys from 8191 bytes down give the right MAC, which I checked against Python's hmac at lengths 0, 1, 20, 63, 64, 65, 127, 128, 129, 200, 1000, and 8191. Debug and release builds fail alike. Hkdf's long salt and Pbkdf2's long password don't hit this, since PSA takes those as HMAC keys inside without an import. I checked both at 10000 bytes against Python.

```
Import std.core
Import std.crypto

// RFC 2104: a key longer than the hash's block is hashed first, so HMAC takes any key.
Function Key(length: Int): UInt8[]
    Local bytes: UInt8[] = []
    For i = 0 Until length
        bytes.Add((i % 251) As UInt8)
    End For
    Return bytes
End Function

Print(Hmac.Sha256(Key(8191), "data") == Hmac.Sha256(Sha256.Hash(Key(8191)), "data"))
Print(Hmac.Sha256(Key(8192), "data") == Hmac.Sha256(Sha256.Hash(Key(8192)), "data"))
```

Expected: True
True
(Python's hmac gives HMAC-SHA256 of the 8192-byte key and "data" as 4f0f1b1d54ae63165a12d13f1a2dda37475acfec700436c97766e6336636c0aa.)

Actual: True
panic: std.crypto's HMAC failed with PSA error -135
error: hmac_any_key.exe exited with code 101, which a panic gives

Suggested fix: In fb_crypto_hmac, when the key is longer than the hash's block (PSA_HASH_BLOCK_LENGTH: 64 bytes for MD5, SHA-1, and SHA-256, 128 for SHA-512), hash it with psa_hash_compute and import the digest instead. RFC 2104 does exactly that, so the MAC is unchanged and every key stays under PSA's limit. Add a crypto_macs.fb check with a 10000-byte key, for example Key(10000) as above with "data" under HMAC-SHA256, which Python's hmac gives as 75d8a72907f932a2820541dca16078156c61fee3b9586afffb28e7f5a0da7661.

Skeptic: Ran C:/Users/Hunter/Projects/FutureBlitz/.claude/worktrees/rev-scratch/s21/hashrefute/hmac_any_key.fb. Debug and --release both printed `True` and then `panic: std.crypto's HMAC failed with PSA error -135` (exit 101, and the panic gives no place). The Sha512 variant (hmac_hashed.fb) panics the same way at 8192 bytes. Hashing the 8192-byte key first gives 4f0f1b1d...6636c0aa, which matches Python's hmac.new(key, b'data', sha256) on the raw key, so RFC 2104 defines that MAC and the suggested fix keeps it the same. The cause: runtime/src/crypto.cpp:238 imports the raw key through hmac_key(), which only swaps an empty key for a zero byte, and PSA_MAX_KEY_BITS is 0xfff8 (tf-psa-crypto/include/psa/crypto_struct.h:302). std/crypto/derive.fb:19 says HMAC takes a key 'of any length'. derive.fb's MacOf checks nothing, so a documented valid input reaches an internal panic.

## S21-keys-1 (medium): Certificate can't read a certificate of an Ed25519 key, though KeyKind has ED25519

Status: fixed in c8be79b: std.crypto reads a certificate's DER itself, so any of its keys reads; crypto_certificates.fb has Ed25519 ones, self-signed and RSA-signed

Where: runtime/src/crypto_pem.cpp:283

mbedTLS 4.1.1's X.509 parser has no Ed25519, so mbedtls_x509_crt_parse refuses any certificate whose SubjectPublicKeyInfo (or signature) is Ed25519. fb_crypto_certificates_new then gives null, and FromPem and AllFromPem throw 'the PEM has no certificate, or one that doesn't read'. KeyKind has ED25519, stdlib.md says Certificate reads the public key, and pem.fb says PublicKey is Null only 'when it's of a kind there's no KeyKind for'. In fact a kind mbedTLS lacks makes the whole certificate unreadable: Ed25519 keys whether the certificate is self-signed or RSA-signed, and RSA keys over 4096 bits (S21-keys-2). Only kinds mbedTLS knows but std doesn't, like P-384, give Null. Because AllFromPem refuses the whole text when one certificate fails, one such certificate in a bundle loses all the others. crypto_pem.fb tests no Ed25519 certificate. (Checked in debug and release.)

```
// Two certificates of Ed25519 keys, which KeyKind has: one openssl req -x509 made from openssl
// genpkey -algorithm ed25519, signed by that key; and one an RSA CA signed (openssl x509 -req
// -force_pubkey), so only its key is Ed25519.
Import std.core
Import std.crypto

Function Read(pem: String)
    Try
        Local certificate = Certificate.FromPem(pem)
        Local kind = certificate.PublicKey?.Kind
        Print("{certificate.Subject}: {kind == KeyKind.ED25519}")
    Catch e As ParseError
        Print("ParseError {e.Kind}: {e.Message}")
    End Try
End Function

Read("-----BEGIN CERTIFICATE-----\n" +
    "MIIBODCB66ADAgECAhRWdLhF1svSCVdFeMZ+j4j6vwYMwTAFBgMrZXAwEjEQMA4G\n" +
    "A1UEAwwHZWQtdGVzdDAeFw0yNjEwMjMwNjE5NDVaFw0yNjExMjIwNjE5NDVaMBIx\n" +
    "EDAOBgNVBAMMB2VkLXRlc3QwKjAFBgMrZXADIQAUIF+Y7bdHi9GuJwxXEtx92QDi\n" +
    "bfWIIKJS1SyX2g4lNaNTMFEwHQYDVR0OBBYEFJAhPs4bixFrkGErMZ7O0e19WkAt\n" +
    "MB8GA1UdIwQYMBaAFJAhPs4bixFrkGErMZ7O0e19WkAtMA8GA1UdEwEB/wQFMAMB\n" +
    "Af8wBQYDK2VwA0EAcJpiNRvyNsUcm2TcvH2p43qC4Yw+TrYh8jf5of3XvejlBlLj\n" +
    "5r4e4UKFDm+RfWTpNwzs556JwgM4ezTLNNdMDQ==\n" +
    "-----END CERTIFICATE-----\n")
Read("-----BEGIN CERTIFICATE-----\n" +
    "MIIB+zCB5KADAgECAhRa1VewXHALElKLNlRTxgsLEdOU7TANBgkqhkiG9w0BAQsF\n" +
    "ADARMQ8wDQYDVQQDDAZyc2EtY2EwHhcNMjYxMDIzMDYyMTQxWhcNMjYxMTIyMDYy\n" +
    "MTQxWjAVMRMwEQYDVQQDDAplZC1zdWJqZWN0MCowBQYDK2VwAyEAFCBfmO23R4vR\n" +
    "ricMVxLcfdkA4m31iCCiUtUsl9oOJTWjQjBAMB0GA1UdDgQWBBSQIT7OG4sRa5Bh\n" +
    "KzGeztHtfVpALTAfBgNVHSMEGDAWgBRZN6imr3Ge506d144bRq+f27NuKjANBgkq\n" +
    "hkiG9w0BAQsFAAOCAQEASsM02CjvEt5ACuJOS/Q1xO1DB/Yau9r1CQShVWWUGhqF\n" +
    "ZDMmvmKEOAxtEa/aoukAUmOivLFjO7alcK/fv9Ox73OMpon0KOjGhi14gcxEyNkr\n" +
    "/1955A7y2YX111vW2WviSOORwK1wSiG24/YDXji84hZokF8jLC/DPz5Jic0JvCAG\n" +
    "pTVseNAa7N8zvVCOVnLVGyFZ3xznoM6SKGlSIwrTG/t948xKHWNMZhCh6AOqP7eR\n" +
    "1v5y9+XdXQQDiJlNxHAAHG11ZwZDe1UWuZ/m/9zR+drkDtNbs7dep0J74toketF4\n" +
    "ETrB+/v7fHFTJTU71qPvQkt2JA9tbDKm+ymh/bRMfQ==\n" +
    "-----END CERTIFICATE-----\n")
```

Expected: CN=ed-test: True
CN=ed-subject: True

Actual: ParseError SYNTAX: the PEM has no certificate, or one that doesn't read
ParseError SYNTAX: the PEM has no certificate, or one that doesn't read

Suggested fix: When mbedTLS refuses a certificate, read its fields in the runtime. A certificate is a SEQUENCE of tbsCertificate, signatureAlgorithm, and signature. Take subject, issuer, serial, and validity with mbedTLS's x509 name and time getters, and the SubjectPublicKeyInfo with the same OIDs curve_key_from_pem knows (1.3.101.112 and 1.3.101.110), giving PublicKey Null for any other kind. Otherwise, say in pem.fb and stdlib.md that a certificate of an Ed25519 key, or of any key mbedTLS lacks, doesn't read. Either way, add a test with an Ed25519 certificate (RFC 8410 10.2 has one, signed by Ed25519).

Skeptic: I ran the finding's program in debug and release, and both printed 'ParseError SYNTAX: the PEM has no certificate, or one that doesn't read' twice. openssl x509 -text reads both certificates: the first is self-signed with an ED25519 signature and key, CN=ed-test; the second has an ED25519 key signed sha256WithRSAEncryption, CN=ed-subject. I also wrote refute-keys/k1b.fb, which runs AllFromPem on a P-256 certificate alone (it gives 1) and then on the same certificate followed by the Ed25519 one, which throws the same SYNTAX error. So one such certificate loses the whole bundle. The code agrees: crypto_pem.cpp:283 treats any nonzero mbedtls_x509_crt_parse result as a failure, and mbedTLS has no Ed25519 X.509 support. This contradicts pem.fb:80 ('Null when it's of a kind there's no KeyKind for'), since KeyKind has ED25519, and stdlib.md's promise that Certificate reads the public key. crypto_pem.fb has no Ed25519 certificate.

## S21-keys-2 (medium): Rsa.Verify gives False for valid signatures by RSA keys over 4096 bits or of an odd bit length; Key.FromPem calls such a key encrypted or unknown

Status: fixed in c8be79b: RSA goes through mbedTLS's RSA module rather than PSA, so odd sizes verify and read, and third_party/ raises PSA's cap to 8192 bits, the most mbedTLS's bignums hold; crypto_rsa_keys.fb has keys of 2047, 4104, and 8192 bits

Where: runtime/src/crypto.cpp:403

TF-PSA-Crypto caps imported RSA keys at PSA_VENDOR_RSA_MAX_KEY_BITS, 4096 in include/psa/crypto_sizes.h. mbedtls_psa_rsa_load_representation() also returns NOT_SUPPORTED for a modulus whose bit length isn't a multiple of 8 (psa_check_rsa_key_byte_aligned). So key_t's psa_import_key fails, fb_crypto_verify returns 0, and Rsa.Verify and VerifyPss quietly say a valid RS256 signature is invalid. Two examples: openssl genrsa 8192 (a valid RSA size, which openssl dgst -verify accepts) and openssl genrsa 2047. Reading such a key also fails. pk_key's mbedtls_pk_import_into_psa fails, so Key.FromPem throws WRONG_TYPE 'the PEM's key is encrypted, or of a kind std.crypto doesn't take', though it's a plain RSA key. A certificate of an 8192-bit key doesn't read either (S21-keys-1). Nothing in signatures.fb, pem.fb, or stdlib.md mentions a size limit. MBEDTLS_MPI_MAX_SIZE is already 1024 bytes, enough for 8192 bits.

```
// openssl genrsa 8192 and 2047, and openssl dgst -sha256 -sign of "hello" with each, which
// openssl dgst -verify verifies.
Import std.core
Import std.crypto

Function Unhex(text: String): UInt8[]
    Local bytes: UInt8[] = []
    Local t = text.Bytes
    For i = 0 Until t.Count / 2
        Local hi = t[i * 2] As Int
        Local lo = t[i * 2 + 1] As Int
        hi = hi >= 97 ? hi - 87 : hi - 48
        lo = lo >= 97 ? lo - 87 : lo - 48
        bytes.Add((hi * 16 + lo) As UInt8)
    End For
    Return bytes
End Function

Local key8192 = Rsa.PublicKey(Unhex("b445de5370c3c710eb6ffeaba8d064335222963863fd9dd31bd6a9a165978b1cbeab4b81aaadd630a11d8c9b90bd69dd85fb042c747eb96eb1612eb8a5d6029a3605c14ebf24abc564c5c80fc8145dce0267687acf7c078d11b0f25383a1c03ec29100b0ebff7a720988e430b596fd2a7ad6606fc133ac8b4f7a5206687583d7925ab44364e5243716bc3f107eaf17b941b4f85b1ad1d80ec01773f0c63a416f6b03d41c0a549983426c1aa8095d1f913cef950f2c74136d4bf38688d5a1a781bd7d8bfcca1c15fc7274cb3fec4af71d70556c3c6ef7b08b8270660d3325bcd1ce34931cf80ec7480722d37ab3d6ee44049ac95dbcfb69ba3ffabdca89e0590ce5f017c4fe69692975a2bfc035919b26c94015f1e3c0e5313b3c2f709e977225141ca168312ad714deea07ebcbed4658ba5940fd8ab5c563151d57f7318dcf36ede77108e52d3c86199e415542a247d5ae12f07b5fce141c93fb77e69bf1a980f7dc0b0e3aca073c8d37f81d9e2b44e48da9afe395545e20f2dc90371354f447e3d2a99c3e339829d8f46b156d903ed30daf3041a881a93ac5a2172efe50c4a3b51189b45e448cd2d2efa8a0c87d8548ddfbfe1a9e74f5fe09b0635cc4cdd0edc9a6a6e21d099074bd9d8eebafa015547b5c0b0b905eec984b848c0fad61f535148063ed71741a1746d2416b260598f35e4226ad9b8e110cea439614fa4c2b04df7e56948f10d1a710a23ec858a991c7b1971dd0f6f00844d2baffa2adcf9ba79b1e1342c9a3311bfaab72e1949dd1f64b650ced72c725a09f3398e53863f5fb5ef595eb9833836475644a082d568da4fbe5b47b3a51022b78991e55a43c150da5514f21e7ee153302f2b92081fbf16e0bd839e271668706e76f7e0e983dd3ddf72bf3ff70f6ab36b85858538832a48b0c9ced482be2c85520f9d7f4a9d342de8e8c92180b23ad641f92ade242a23f32e8e6c5e045067685815e3225921f7cc3e3227c2b6e21a1b0d82b185ddd2d01740d430d1b2bf60f96ec4f30da3decca64bd015549d26cadaf87e46a58cb71cfd15287ee705aa75a1a6f97a75f193f542aa712e25768f2ad698aff9d5bdcd7dd67684e5d69e2daa017b851efc6bef889e33b6c5dd6c3e1d52ec1dcbbf25f59445780740d023fdd8cd0960da7f6ebdffcce77e90d1a5322ec317da1fe7e07c17b6132a445e9309bbe907694789acea3918c83a897caff3d84351a2b0eb79ba89b5678de365c670c75b561ac2d96077ac137a678c2cd639050306ec8b44caa72f54fb182cdcd38eb360a2b61296feab005e2a850d1c6cf3e0402c5ca244dc29b9fe1c5a55c3058040c4fa48833839eb000b4c80e0b4f88eef9a77ed6c6c6e88b7126a0e17a31e5c11f94df3b0e087920b4fbd734451b6f3128068b5fb2dc4d1b3b6faa8ea6b12c5ec3bd3c7ed2c7d6de607b"), Unhex("010001"))
Print(Rsa.Verify(key8192, "hello", Unhex("337d4fe555039fc7420b141d47867d2da47fc52e3259d3e042873e3620bb7bb65edc0651df154ec8bca11bff62b5f6422e17596153c720d1c68a00208ca5e5a5da059fdead86a772ce20b835c2b49f7749f5dce01b8de8f5f3c7022b097aaa5348cd632e80fc59cd11703ec031490c08217a55bb0d6154b9c59c486723e6577a7cc0b4bbc1e186d155e8a9f5290a933b22786d2490ec362c9f9f1fcd92ea2bbdc9c43be4270ba611caf59f78baa31ff4e50e5c4bd1d0537b3df179f785ce970198b31802efa58886836da79f48a6e2622f4c39b14b205aa2073dde3b388bb4c35fc08a0ce5ff81e11d815835e9a582141d2cc0e8a16b17b256b14b0ab871ef7cafa149fc038743e992589024a8700adfbb0b6139cc5ce6f0eba6f80f7ff43fbc203b185c815d8b82d50767821ec3f7a7778502e16823db6b9c45267bc9f454df0a7b7d7445072ce1146cc0b53e61d92ac5a2fcc52002fdcf9ba243856aa8d2dc57576a404e88ae33ed69604a5e7f75f32c65e1bae06eb56d9cd4e39139403a86f23c5e266f3f5cbbcf186c53b1857718090149d5846c5eeb57d986df185b108ac3700c2b316e9159d26c5a43a27e37387f7627ea9642db61b4158fc6b4be1a6723b6e015bb08c7076929a634acfa43ed0f137592fb2ca2a4a62f82ae65ba020527343d65f29283b61faea945180ec18b854f33af8defe18ff84526027561877c2aa17434ddc8d3972969863d3839bef6b0d50d11cf78a0b8034d0d450e4925238fbbee3efd601bd4c0bf54faf5fe2a2e4ffff851f0a00b19dcd1ee13213ef0fcbadc725c2d265c320915a63657bef2db2aaaef6a282564519421d0c2f0d892033446abbc8a873e5c11865fa8bbfc427f250f9bca46a6070e39c0a9292cbeb98eeb7d1f23bc305a2b8d75786173f63b5ed865aedd2d6ab0e48b6888f4dd58f301b21806c86b19677bb8b5770bdbe098fdc53bdd8233a473cea68a15dc5b5028062ae33ab4596e90966f38339ab1e2049d5a08a22491efbeccfa8a6a1233143d40a805ef6836c9f2df4b179798cf6e940b9976dbbb20bb64bf94390f8908b419401d30ccc7f378cec8fb135707e02c10663e6de7c090866948408009ccfaccaba76113e73c614366fece21547b663f985d8fe3bfb77b4fc9258f987a86fd8c91e80cfb591c47740bfd9ab1713b3f7f4d8e6fc969c19a478085aabab144db83fa0df719e910cc78b5af264ec7ced093c4a39caa9d61da4a1e55bbcdf4aefcbc3559ac0febd06b653bf78379e476fb6a4858219174469ed27f4e6e23489540d64c120dc2d09aa2686dc411e9c38b60828bf0fc0b6e572bd32278571ee312cc08bacd402bd888b03a5a9818ce0ea5db14acde4a3eeadf503ba4ca6a7ebe443c7f03778530cf3db971524e99c5670d3c7d6d27bb7dbf06bf6ad8257dd09ac2269e0639")))
Local key2047 = Rsa.PublicKey(Unhex("5ee3a70f877dc87e36696a41179de3fe8a2a5ef5b31e61f03d10c9fdb0f8a05da02e98a6b95ac9d81611bfc6e6a75ece39f5933d01ab4897ffd22a2e19026f03fb93d51b27dfa9bd07408daf430673a824ea166aff9b4f192fe626fde2018e184adc3cac35d8858cad65f9d56c1b57510527c7f0e13b66f6a77fa09925f61b07500ebf7527de9519aa58513b5e9bafd7a5b19293372ae643e34b361ec8a1ef3e1474a1c29e2cf60f73164fe0f74289a0817cde3a36d8158118f00a328f332690cc3b2a3fe5a72f37626de6d9530fb75548e3833dcfb0536b54fa050fefa8854d0bb01b95ade6d0b9ecd927e1199e9c63bae4bf976efc50183dcc13afce9cc76f"), Unhex("010001"))
Print(Rsa.Verify(key2047, "hello", Unhex("55fde2aa9ca7b30a39bd3b549f95d6dea58c43c371ef5131eab832afd627b13c3c8eabf38d99fc7f0192bb4495cfca3953411c2c3dceb7fcc2fee7950522ad8ac3c7a1fd3cad4406b2194b9b4d1a84cc92adab8e74e3f8b7b26c108eaa3862d44a558e24e2a20c10867e0b0904dccdbdb8e53aef8e791fbf2b8b03d3632f07a727f569789cecba94f1d1b7e92dd2e12eff2a56afe439d8ad490f7969ecb51ba8bcce8f81c33ac8f605fce53eba26c2360b0f2383902707db9f8b497c8e25e1c32467d07ebcde23c31ed6aabacd92472cab716dda7716919150f4325cb772ed74da04651b79995f434247aa54a567cff6f40eabdbf1221ffccb99adee3ef2cfdd")))
Try
    Key.FromPem("-----BEGIN PUBLIC KEY-----\n" +
        "MIIEIjANBgkqhkiG9w0BAQEFAAOCBA8AMIIECgKCBAEAtEXeU3DDxxDrb/6rqNBk\n" +
        "M1Iiljhj/Z3TG9apoWWXixy+q0uBqq3WMKEdjJuQvWndhfsELHR+uW6xYS64pdYC\n" +
        "mjYFwU6/JKvFZMXID8gUXc4CZ2h6z3wHjRGw8lODocA+wpEAsOv/enIJiOQwtZb9\n" +
        "KnrWYG/BM6yLT3pSBmh1g9eSWrRDZOUkNxa8PxB+rxe5QbT4WxrR2A7AF3PwxjpB\n" +
        "b2sD1BwKVJmDQmwaqAldH5E875UPLHQTbUvzhojVoaeBvX2L/MocFfxydMs/7Er3\n" +
        "HXBVbDxu97CLgnBmDTMlvNHONJMc+A7HSAci03qz1u5EBJrJXbz7abo/+r3KieBZ\n" +
        "DOXwF8T+aWkpdaK/wDWRmybJQBXx48DlMTs8L3Cel3IlFByhaDEq1xTe6gfry+1G\n" +
        "WLpZQP2KtcVjFR1X9zGNzzbt53EI5S08hhmeQVVCokfVrhLwe1/OFByT+3fmm/Gp\n" +
        "gPfcCw46ygc8jTf4HZ4rROSNqa/jlVReIPLckDcTVPRH49KpnD4zmCnY9GsVbZA+\n" +
        "0w2vMEGogak6xaIXLv5QxKO1EYm0XkSM0tLvqKDIfYVI3fv+Gp509f4JsGNcxM3Q\n" +
        "7cmmpuIdCZB0vZ2O66+gFVR7XAsLkF7smEuEjA+tYfU1FIBj7XF0GhdG0kFrJgWY\n" +
        "815CJq2bjhEM6kOWFPpMKwTfflaUjxDRpxCiPshYqZHHsZcd0PbwCETSuv+irc+b\n" +
        "p5seE0LJozEb+qty4ZSd0fZLZQztcscloJ8zmOU4Y/X7XvWV65gzg2R1ZEoILVaN\n" +
        "pPvltHs6UQIreJkeVaQ8FQ2lUU8h5+4VMwLyuSCB+/FuC9g54nFmhwbnb34OmD3T\n" +
        "3fcr8/9w9qs2uFhYU4gypIsMnO1IK+LIVSD51/Sp00LejoySGAsjrWQfkq3iQqI/\n" +
        "MujmxeBFBnaFgV4yJZIffMPjInwrbiGhsNgrGF3dLQF0DUMNGyv2D5bsTzDaPezK\n" +
        "ZL0BVUnSbK2vh+RqWMtxz9FSh+5wWqdaGm+Xp18ZP1QqpxLiV2jyrWmK/51b3Nfd\n" +
        "Z2hOXWni2qAXuFHvxr74ieM7bF3Ww+HVLsHcu/JfWURXgHQNAj/djNCWDaf269/8\n" +
        "znfpDRpTIuwxfaH+fgfBe2EypEXpMJu+kHaUeJrOo5GMg6iXyv89hDUaKw63m6ib\n" +
        "VnjeNlxnDHW1Yawtlgd6wTemeMLNY5BQMG7ItEyqcvVPsYLNzTjrNgorYSlv6rAF\n" +
        "4qhQ0cbPPgQCxcokTcKbn+HFpVwwWAQMT6SIM4OesAC0yA4LT4ju+ad+1sbG6Itx\n" +
        "JqDhejHlwR+U3zsOCHkgtPvXNEUbbzEoBotfstxNGztvqo6msSxew708ftLH1t5g\n" +
        "ewIDAQAB\n" +
        "-----END PUBLIC KEY-----\n")
    Print("read")
Catch e As ParseError
    Print("ParseError {e.Kind}: {e.Message}")
End Try
```

Expected: True
True
read

Actual: False
False
ParseError WRONG_TYPE: the PEM's key is encrypted, or of a kind std.crypto doesn't take

Suggested fix: Raise PSA_VENDOR_RSA_MAX_KEY_BITS in third_party/mbedtls_config.h, to 8192 at least (MBEDTLS_MPI_MAX_SIZE already allows it). PSA can't take odd-length moduli, so verify those through mbedTLS's RSA module directly, or document that they're refused. Document the size range on Rsa and Key.FromPem, give Key.FromPem a message saying the key's size isn't supported, and add an 8192-bit RS256 check.

Skeptic: I generated my own keys with openssl genrsa at 2047, 2048, 4096, 4104, and 8192 bits and signed "hello" with openssl dgst -sha256 -sign; openssl dgst -verify says 'Verified OK' for all five. refute-keys/k2.fb (made by gen.py) checks each through Rsa.PublicKey(modulus, 65537) and through Key.FromPem. In debug and release, 2048 and 4096 read and verify True. 2047, 4104, and 8192 give 'by modulus: False' and 'ParseError WRONG_TYPE: the PEM's key is encrypted, or of a kind std.crypto doesn't take'. So both the over-4096 cap and the non-byte-aligned refusal are real, and the failure is silent. fb_crypto_verify (crypto.cpp:403) returns 0 when key_t's import fails. signatures.fb's Rsa comments and stdlib.md give no size limit. (Before moving to s21/refute-keys/, my first run wrote k1.fb, k2.fb, and some RSA test files into s21/keys/, the reviewer's folder.)

## S21-keys-3 (medium): Key.FromPem doesn't give the first key in the PEM

Status: fixed in c8be79b

Where: runtime/src/crypto_pem.cpp:216

pem.fb says FromPem gives 'The first key in pem'. fb_crypto_key_from_pem first calls mbedtls_pk_parse_key, which searches the whole text for 'RSA PRIVATE KEY', then 'EC PRIVATE KEY', then 'PRIVATE KEY'. Only after that fails does it call mbedtls_pk_parse_public_key, and only after both fail does it call curve_key_from_pem. So a private key anywhere wins over an earlier public key. An 'RSA PRIVATE KEY' wins over an earlier 'EC PRIVATE KEY' (also tested: EC PRIVATE KEY, then RSA PRIVATE KEY, gives RSA). Any key mbedTLS reads wins over an earlier Ed25519 or X25519 key. A program reading a file of several keys gets a different key, with a different Kind or IsPrivate, from the one documented.

```
// Key.FromPem says it reads the first key in the PEM.
Import std.core
Import std.crypto

Function Show(pem: String)
    Try
        Local key = Key.FromPem(pem)
        Print("{key.Kind}, private: {key.IsPrivate}")
    Catch e As ParseError
        Print("ParseError {e.Kind}: {e.Message}")
    End Try
End Function

// A P-256 public key, then a private one.
Show("-----BEGIN PUBLIC KEY-----\n" +
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAE7fjQvYW6EbS12khag4DR50TeVGnQ\n" +
    "shZ9zqOhcohtENjhAtz48fWAh9Vyb70wSGpUU95N77iC16vx+JX6Cq+Wsw==\n" +
    "-----END PUBLIC KEY-----\n" +
    "-----BEGIN PRIVATE KEY-----\n" +
    "MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQg+5kwlov2G2geVvtD\n" +
    "6T5+2FOzjzOnhww0IiXreSqkzpmhRANCAATt+NC9hboRtLXaSFqDgNHnRN5UadCy\n" +
    "Fn3Oo6FyiG0Q2OEC3Pjx9YCH1XJvvTBIalRT3k3vuILXq/H4lfoKr5az\n" +
    "-----END PRIVATE KEY-----\n")
// An Ed25519 private key, then a P-256 public one.
Show("-----BEGIN PRIVATE KEY-----\n" +
    "MC4CAQAwBQYDK2VwBCIEIAMpMwWs7Xq0KatoYeKFjsxH6ezRfGNnJOGlUsStLXHY\n" +
    "-----END PRIVATE KEY-----\n" +
    "-----BEGIN PUBLIC KEY-----\n" +
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAE7fjQvYW6EbS12khag4DR50TeVGnQ\n" +
    "shZ9zqOhcohtENjhAtz48fWAh9Vyb70wSGpUU95N77iC16vx+JX6Cq+Wsw==\n" +
    "-----END PUBLIC KEY-----\n")
```

Expected: ECDSA_P256, private: False
ED25519, private: True

Actual: ECDSA_P256, private: True
ECDSA_P256, private: False

Suggested fix: Find the first '-----BEGIN <label>-----' whose label ends in ' KEY' and cut out that block, up to its matching END line. Give only that block to mbedtls_pk_parse_key or mbedtls_pk_parse_public_key (by its label) and to curve_key_from_pem. Or change the comment to say which key it picks. Add a test of two keys in one PEM.

Skeptic: I ran refute-keys/k3.fb, the finding's two cases plus each key alone as a control, in debug and release. A P-256 PUBLIC KEY followed by a PRIVATE KEY gives 'ECDSA_P256, private: True'. An Ed25519 PRIVATE KEY followed by a P-256 PUBLIC KEY gives 'ECDSA_P256, private: False'. The controls read correctly ('ECDSA_P256, private: False' and 'ED25519, private: True'). fb_crypto_key_from_pem (crypto_pem.cpp:216-220) tries mbedtls_pk_parse_key, then mbedtls_pk_parse_public_key, then curve_key_from_pem, each over the whole text, so the choice goes by kind, not position. pem.fb:43 says 'The first key in pem'. The wrong key comes back silently.

## S21-keys-4 (medium): Certificate names: non-ASCII becomes hex escapes, T61/BMP strings become #DER hex, and a long value makes the whole Subject empty

Status: fixed in c8be79b: names are written from the DER, as UTF-8 with RFC 4514's escapes, in a std::string

Where: runtime/src/crypto_pem.cpp:192

name_of() returns mbedtls_x509_dn_gets() as it is, and that is not the readable 'C=NL, O=Example, CN=example.com' text pem.fb promises once a name isn't plain ASCII. dn_gets writes every byte outside 32..126 as \XX, so a UTF8String 'Zürich' comes out as 'Z\C3\BCrich'. A value that isn't a UTF8String, PrintableString, or IA5String (T61String, BMPString) comes out as '#' plus the hex of its DER. dn_gets also builds each value in a 256-byte buffer (MBEDTLS_X509_MAX_DN_NAME_SIZE) and fails with BUFFER_TOO_SMALL once the escaped text passes that, and name_of turns any failure into "" without saying so. So a certificate whose O is 35 Japanese characters (105 UTF-8 bytes, 315 escaped) has an empty Subject and Issuer. Also shown: an all-ASCII subject over 1023 characters (18 OUs of 60 characters) gives "", because name_of's own buffer is 1024 bytes. Python's ssl and openssl -nameopt utf8 show these names as text.

```
// Certificates made with openssl req -config, utf8 = yes and string_mask = utf8only, whose
// names are UTF8Strings: CN=Zürich Straße, O=Ünïcode; and C=JP, O=35 Japanese characters,
// CN=example.jp; and, with string_mask = default, the first one's names as T61Strings.
Import std.core
Import std.crypto

Function Names(pem: String)
    Try
        Local certificate = Certificate.FromPem(pem)
        Print("[{certificate.Subject}] [{certificate.Issuer}]")
    Catch e As ParseError
        Print("ParseError {e.Kind}: {e.Message}")
    End Try
End Function

Names("-----BEGIN CERTIFICATE-----\n" +
    "MIIBfzCCASWgAwIBAgIUH9kHGq+ABPHdX0Kywec9ZZIdF0IwCgYIKoZIzj0EAwIw\n" +
    "LjEYMBYGA1UEAwwPWsO8cmljaCBTdHJhw59lMRIwEAYDVQQKDAnDnG7Dr2NvZGUw\n" +
    "HhcNMjYxMDIzMDYyMjMzWhcNMjYxMTIyMDYyMjMzWjAuMRgwFgYDVQQDDA9aw7xy\n" +
    "aWNoIFN0cmHDn2UxEjAQBgNVBAoMCcOcbsOvY29kZTBZMBMGByqGSM49AgEGCCqG\n" +
    "SM49AwEHA0IABO340L2FuhG0tdpIWoOA0edE3lRp0LIWfc6joXKIbRDY4QLc+PH1\n" +
    "gIfVcm+9MEhqVFPeTe+4gter8fiV+gqvlrOjITAfMB0GA1UdDgQWBBT/sIXuWHvP\n" +
    "b1+cG3nBe6T7AzYbizAKBggqhkjOPQQDAgNIADBFAiB1Ck8NY9aiaidwKB/efd31\n" +
    "f1LkCrarzwUKq65dNv37owIhAO7xo+wFWDLKnBV2DmNsQW0p+0ZxLg1GHbY2YC/f\n" +
    "Yedh\n" +
    "-----END CERTIFICATE-----\n")
Names("-----BEGIN CERTIFICATE-----\n" +
    "MIICUTCCAfegAwIBAgIUf+m2Z/Oc8EW80FZjmvHxV6ZxdpMwCgYIKoZIzj0EAwIw\n" +
    "gZYxCzAJBgNVBAYTAkpQMXIwcAYDVQQKDGnml6XmnKzoqp7jga7kvJrnpL7lkI3m\n" +
    "l6XmnKzoqp7jga7kvJrnpL7lkI3ml6XmnKzoqp7jga7kvJrnpL7lkI3ml6XmnKzo\n" +
    "qp7jga7kvJrnpL7lkI3ml6XmnKzoqp7jga7kvJrnpL7lkI0xEzARBgNVBAMMCmV4\n" +
    "YW1wbGUuanAwHhcNMjYxMDIzMDYyNjQ2WhcNMjYxMTIyMDYyNjQ2WjCBljELMAkG\n" +
    "A1UEBhMCSlAxcjBwBgNVBAoMaeaXpeacrOiqnuOBruS8muekvuWQjeaXpeacrOiq\n" +
    "nuOBruS8muekvuWQjeaXpeacrOiqnuOBruS8muekvuWQjeaXpeacrOiqnuOBruS8\n" +
    "muekvuWQjeaXpeacrOiqnuOBruS8muekvuWQjTETMBEGA1UEAwwKZXhhbXBsZS5q\n" +
    "cDBZMBMGByqGSM49AgEGCCqGSM49AwEHA0IABO340L2FuhG0tdpIWoOA0edE3lRp\n" +
    "0LIWfc6joXKIbRDY4QLc+PH1gIfVcm+9MEhqVFPeTe+4gter8fiV+gqvlrOjITAf\n" +
    "MB0GA1UdDgQWBBT/sIXuWHvPb1+cG3nBe6T7AzYbizAKBggqhkjOPQQDAgNIADBF\n" +
    "AiBxYpZNkT+ANvOEJvJSIB1PPEIAm/CJfxRsebyAhvq0PAIhAOyjXGg1n//KyVY1\n" +
    "TErAwTHNtYSS75VN5XMyVF+moljk\n" +
    "-----END CERTIFICATE-----\n")
Names("-----BEGIN CERTIFICATE-----\n" +
    "MIIBeDCCAR2gAwIBAgIURx5Cl2l83YLsvE/FvW7/PQydzHcwCgYIKoZIzj0EAwIw\n" +
    "KjEWMBQGA1UEAxQNWvxyaWNoIFN0cmHfZTEQMA4GA1UEChQH3G7vY29kZTAeFw0y\n" +
    "NjEwMjMwNjI2NTlaFw0yNjExMjIwNjI2NTlaMCoxFjAUBgNVBAMUDVr8cmljaCBT\n" +
    "dHJh32UxEDAOBgNVBAoUB9xu72NvZGUwWTATBgcqhkjOPQIBBggqhkjOPQMBBwNC\n" +
    "AATt+NC9hboRtLXaSFqDgNHnRN5UadCyFn3Oo6FyiG0Q2OEC3Pjx9YCH1XJvvTBI\n" +
    "alRT3k3vuILXq/H4lfoKr5azoyEwHzAdBgNVHQ4EFgQU/7CF7lh7z29fnBt5wXuk\n" +
    "+wM2G4swCgYIKoZIzj0EAwIDSQAwRgIhAPKfo7k1F29II+KNr4LFPpDGpWBlengS\n" +
    "+Oihk/3DKmPvAiEAiaI06HloLNUHp4S4UoIUKCMHwtqgkYU8GUhq+gB3oqA=\n" +
    "-----END CERTIFICATE-----\n")
```

Expected: [CN=Zürich Straße, O=Ünïcode] [CN=Zürich Straße, O=Ünïcode]
[C=JP, O=日本語の会社名日本語の会社名日本語の会社名日本語の会社名日本語の会社名, CN=example.jp] [C=JP, O=日本語の会社名日本語の会社名日本語の会社名日本語の会社名日本語の会社名, CN=example.jp]
[CN=Zürich Straße, O=Ünïcode] [CN=Zürich Straße, O=Ünïcode]

Actual: [CN=Z\C3\BCrich Stra\C3\9Fe, O=\C3\9Cn\C3\AFcode] [CN=Z\C3\BCrich Stra\C3\9Fe, O=\C3\9Cn\C3\AFcode]
[] []
[CN=#140D5AFC726963682053747261DF65, O=#1407DC6EEF636F6465] [CN=#140D5AFC726963682053747261DF65, O=#1407DC6EEF636F6465]

Suggested fix: Format names in the runtime and stop using mbedtls_x509_dn_gets. Walk the mbedtls_x509_name list, name each attribute with mbedtls_x509_oid_get_attr_short_name (or the dotted OID), and decode each value to UTF-8 by its tag: UTF8String, PrintableString, and IA5String as they are, BMPString from UTF-16BE, UniversalString from UTF-32BE, and T61String as Latin-1, as OpenSSL does. Keep RFC 4514's escapes only for , + " \ < > ; and a leading # or space, use ' + ' for multi-valued RDNs, and write into a std::string so there's no length limit. Add tests with UTF-8 and long names.

Skeptic: I ran refute-keys/k4.fb, the finding's three certificates, in debug and release. The UTF8String one gives '[CN=Z\C3\BCrich Stra\C3\9Fe, O=\C3\9Cn\C3\AFcode]'. The T61String one gives '[CN=#140D5AFC726963682053747261DF65, O=#1407DC6EEF636F6465]'. The Japanese-O one gives '[] []', an empty Subject and Issuer. openssl reads all three: 'subject=CN=Zürich Straße, O=Ünïcode'; asn1parse shows T61STRING for the third; and 'subject=C=JP, O=日本語の会社名…, CN=example.jp'. name_of() (crypto_pem.cpp:190-194) passes mbedtls_x509_dn_gets through as it is and turns any error into "". So a name too long for mbedTLS's per-value buffer, or for the 1024-byte one here, silently becomes empty, against pem.fb's readable 'C=NL, O=Example, CN=example.com' text.

## S21-seal-1 (medium): mbedTLS is built at -O0 -g3 in a Debug build, though third_party says every bundled library is optimized

Status: fixed in 7ba4cbb: driver_tests.cpp checks compile_commands.json

Where: third_party/CMakeLists.txt:97

fb_make_available(mbedtls) appends `-w -O2` to CMAKE_C_FLAGS. But mbedTLS's own CMake adds `$<$<CONFIG:Debug>:-O0 -g3>` as target options to each of its libraries (mbedtls-src/tf-psa-crypto/CMakeLists.txt:283 for GCC and :318 for Clang, and mbedtls-src/CMakeLists.txt:231/266 for mbedx509), and target options come after CMAKE_C_FLAGS on the command line. So the last -O wins, and in a Debug build of fb (the one CI and ctest use, on all three OSes) tfpsacrypto, its builtin drivers, and mbedx509 are unoptimized and carry macro debug info. In build-rev2's compile_commands.json, gcm.c, sha256.c, psa_crypto.c and x509_crt.c all compile with `-w -O2 -mcrtdll=ucrt -g -std=c99 ... -O0 -g3`. xxHash, Argon2, crypt_blowfish, Monocypher and BLAKE3 really do get -O2. This breaks the step's call that bundled libraries build with -O2 in every build, and the comment at line 46. It also breaks the 'about 500 KB' growth: -g3's .debug_macro is about 8.6 MB, and fb doesn't strip release programs.

```
// rnd.fb, built with `fb build --release rnd.fb -o r_rnd.exe` by build-rev2's (Debug) fb, beside
// hello.fb (Import std.core, Print("hi")) built the same way; then `objdump -h r_rnd.exe` and
// `objdump -d r_rnd.exe` at <mbedtls_sha256_update>.
Import std.core
Import std.crypto

Print(SecureRandom.Bytes(4).Count)
```

Expected: Prints 4. mbedTLS's objects are compiled -O2 as third_party/CMakeLists.txt says, and r_rnd.exe is about 500 KB bigger than r_hello.exe.

Actual: Prints 4, but r_rnd.exe is 12,975,616 bytes and r_hello.exe is 2,761,216. Of that, 0x83efca (8,646,602) bytes are .debug_macro, which r_hello.exe doesn't have. Stripped, they differ by 516 KB. mbedtls_sha256_update disassembles as -O0 code: `push %rbp; mov %rsp,%rbp; sub $0x40,%rsp; mov %rcx,0x10(%rbp); mov %rdx,0x18(%rbp); mov %r8,0x20(%rbp); ...`, with every argument spilled to the frame. compile_commands.json shows `... -O2 ... -O0 -g3 ...` for every mbedTLS file.

Suggested fix: After fb_make_available(mbedtls), append the options to each of mbedTLS's library targets so they come after its own, e.g. walk BUILDSYSTEM_TARGETS of ${mbedtls_SOURCE_DIR} and its subdirectories and call target_compile_options(${t} PRIVATE -O2 -g0) (or -g1) for each C library target: tfpsacrypto, builtin, mbedx509, and the driver object libraries. Add a check, for example in verify.py, that no bundled object compiles with a later -O0.

Skeptic: build-rev2 is a Debug build (CMakeCache CMAKE_BUILD_TYPE=Debug), and its compile_commands.json uses rev-crypto's mbedtls_config.h. Every mbedTLS file (116 of them: 83 in tf-psa-crypto, 33 in library/, gcm.c, sha256.c, x509_crt.c among them) compiles with `-w -O2 -mcrtdll=ucrt -g -std=c99 ... -O0 -g3 -Wmissing-declarations`, so the later -O0 wins. No other bundled library gets -O0. That comes from mbedTLS's own `target_compile_options(${target} PRIVATE $<$<CONFIG:Debug>:-O0 -g3>)` at tf-psa-crypto/CMakeLists.txt:283/318 and CMakeLists.txt:231/266. xxhash.c compiles `-g -w -O2 -mcrtdll=ucrt`, as intended. In seal-verify/, `fb build --release` of rnd.fb (SecureRandom.Bytes(4)) prints 4, and gives r_rnd.exe of 12,975,616 bytes against r_hello.exe's 2,761,216. objdump -h shows .debug_macro 0x83efca (8.6 MB) in r_rnd.exe only. mbedtls_sha256_update disassembles as -O0 code: `push %rbp; mov %rsp,%rbp; sub $0x40,%rsp; mov %rcx,0x10(%rbp); mov %rdx,0x18(%rbp); mov %r8,0x20(%rbp)`. This contradicts third_party/CMakeLists.txt:46's 'optimized in every build' and the lane's -O2 call. Only Debug builds of fb are affected; in Release, mbedTLS adds -O2 itself. Those Debug builds are the ones CI and ctest use.

## S21-hash-2 (low): crypto_macs.fb says it checks RFC 7914's PBKDF2-HMAC-SHA256 vectors but has none; Pbkdf2.Sha256, Pbkdf2.Sha512, and Hkdf.Sha512 meet no vector

Status: fixed in 7b408c5, the vectors; the code was right, so they pass before it too

Where: tests/programs/crypto_macs.fb:95

The done-when is "every algorithm matches its published test vectors". crypto_macs.fb's header (line 2) and the comment at line 95 both say it checks RFC 7914's PBKDF2-HMAC-SHA256 vectors, but the PBKDF2 section only calls Pbkdf2.Sha1 (RFC 6070). No test checks Pbkdf2.Sha256's or Pbkdf2.Sha512's output: the only other call is in panic_crypto_pbkdf2_iterations.fb, which prints only the Count. Hkdf.Sha512 is called only in no test at all. The code is right: the program below prints True three times, and 207 more comparisons against Python's hashlib and hmac (HKDF and PBKDF2 for every hash, odd lengths, empty inputs) all matched. But nothing in the suite would catch a regression.

```
Import std.core
Import std.collections
Import std.crypto

Function Hex(bytes: UInt8[]): String
    Local digits = ["0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "a", "b", "c", "d", "e", "f"]
    Local parts: String[] = []
    For b In bytes
        parts.Add(digits[(b >> 4) As Int] + digits[(b & 15) As Int])
    End For
    Return parts.Join("")
End Function

// RFC 7914 section 11's PBKDF2-HMAC-SHA256 vectors, which crypto_macs.fb's header says it
// checks, and a PBKDF2-HMAC-SHA512 one; no test calls Pbkdf2.Sha256's or Sha512's results.
Print(Hex(Pbkdf2.Sha256("passwd", "salt".Bytes, 1, 64)) == "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783")
Print(Hex(Pbkdf2.Sha256("Password", "NaCl".Bytes, 80000, 64)) == "4ddcd8f60b98be21830cee5ef22701f9641a4418d04c0414aeff08876b34ab56a1d425a1225833549adb841b51c9b3176a272bdebba1d078478f62b397f33c8d")
Print(Hex(Pbkdf2.Sha512("password", "salt".Bytes, 1, 64)) == "867f70cf1ade02cff3752599a3a53dc4af34c7a669815ae5d513554e1c8cf252c02d470a285a0501bad999bfe943c08f050235d7d68b1da55e63f73b60a57fce")
```

Expected: crypto_macs.fb checks these RFC 7914 vectors, as its header says, plus a vector for each of Pbkdf2.Sha512 and Hkdf.Sha512.

Actual: crypto_macs.fb has no RFC 7914 check, and no Pbkdf2.Sha256, Pbkdf2.Sha512, or Hkdf.Sha512 vector. Run alone, the program prints True, True, True.

Suggested fix: Add the two RFC 7914 PBKDF2-HMAC-SHA256 vectors and a PBKDF2-HMAC-SHA512 one to crypto_macs.fb's PBKDF2 section, and add an HKDF-SHA512 one, such as Wycheproof's or RFC 5869 case 1's inputs under SHA-512 with an independently computed result. Or reword the header so it claims only what it checks.

Skeptic: tests/programs/crypto_macs.fb lines 1-2 and 95 say it checks RFC 7914's PBKDF2-HMAC-SHA256 vectors, but its PBKDF2 section (lines 96-101) has only the five Pbkdf2.Sha1 RFC 6070 checks. A grep of tests/ finds Pbkdf2.Sha256 only in panic_crypto_pbkdf2_iterations.fb, which prints the Count. It finds no Pbkdf2.Sha512 or Hkdf.Sha512 call anywhere. The milestones row 21 done-when asks that every algorithm match its published test vectors. The reviewer's program (pbkdf2_vec.fb) prints True three times, so the code is right, but the test header claims more than it checks and three entry points meet no vector.

## S21-hash-3 (low): stdlib.md's module table and derive.fb's header say Hkdf is by SHA-1 too, but it has only Sha256 and Sha512

Status: fixed in 85d55f7, which is docs, so no test can fail without it

Where: docs/stdlib.md:72

docs/stdlib.md's module table says std/crypto/derive.fb has "Hmac, Hkdf, and Pbkdf2, each by SHA-1, SHA-256, or SHA-512", and derive.fb's header comment (lines 1-2) says the same. Hkdf has only Sha256 and Sha512. stdlib.md's std.crypto section gets this right ("Hkdf.Sha256 and Sha512").

```
Import std.core
Import std.crypto

// stdlib.md's table and derive.fb's header say Hmac, Hkdf, and Pbkdf2 are each by SHA-1,
// SHA-256, or SHA-512.
Local material: UInt8[] = [1, 2, 3]
Local none: UInt8[] = []
Print(Hkdf.Sha1(material, none, none, 20).Count)
```

Expected: 20, as the docs say, or docs that don't claim an Hkdf by SHA-1.

Actual: hkdf_sha1.fb:8:12: error: Hkdf has no member named 'Sha1'

Suggested fix: Say "Hmac and Pbkdf2 by SHA-1, SHA-256, or SHA-512, and Hkdf by SHA-256 or SHA-512" in stdlib.md line 72 and in derive.fb's header.

Skeptic: hkdf_sha1.fb fails with `Hkdf has no member named 'Sha1'`. docs/stdlib.md:72's module table and std/crypto/derive.fb's header (lines 1-2) both say Hmac, Hkdf, and Pbkdf2 are 'each by SHA-1, SHA-256, or SHA-512'. derive.fb defines only Hkdf.Sha256 and Sha512, and stdlib.md's own std.crypto section (line 405) says 'Hkdf.Sha256 and Sha512'. So the table and the file comment are wrong about the code and contradict the design section. It's a documentation fix.

## S21-keys-5 (low): Key.FromPem changes an X25519 private key's bytes (clamps them), so ToPem doesn't give back the PEM it read

Status: fixed in c8be79b

Where: runtime/src/crypto_pem.cpp:154

An X25519 PKCS #8 key goes through mbedtls_pk_parse_key. mbedtls_ecp_read_key clamps the Curve25519 scalar (clears bits 0-2 and 255, sets 254), and pk_key exports that clamped scalar. Key.Bytes therefore aren't the 32 bytes in the PEM: RFC 7748's Alice key 77076d0a...1db92c2a reads as 70076d0a...1db92c6a. X25519 results are the same, since X25519 clamps anyway. But the Key doesn't equal one made from the PEM's bytes, and FromPem then ToPem gives different text. crypto_pem.fb's 'keys of each kind go to PEM and back' only uses X25519.NewPrivateKey(), which is clamped already, so it doesn't catch this. OpenSSL clamps its own new keys too, but other tools' keys, and RFC 7748's, aren't clamped.

```
// RFC 7748's Alice's private key (6.1), 77076d0a...1db92c2a, as RFC 8410's PKCS #8.
Import std.core
Import std.collections
Import std.crypto

Function Hex(bytes: UInt8[]): String
    Local digits = ["0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "a", "b", "c", "d", "e", "f"]
    Local parts: String[] = []
    For b In bytes
        parts.Add(digits[(b >> 4) As Int] + digits[(b & 15) As Int])
    End For
    Return parts.Join("")
End Function

Local pem = "-----BEGIN PRIVATE KEY-----\n" +
    "MC4CAQAwBQYDK2VuBCIEIHcHbQpzGKV9PBbBclGyZkXfTC+H68CZKrF3+6UduSwq\n" +
    "-----END PRIVATE KEY-----\n"
Try
    Local key = Key.FromPem(pem)
    Print(Hex(key.Bytes))
    Print(key.ToPem() == pem)
Catch e As ParseError
    Print("ParseError {e.Kind}: {e.Message}")
End Try
```

Expected: 77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a
True

Actual: 70076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c6a
False

Suggested fix: Read X25519 (and Ed25519) PKCS #8 and SubjectPublicKeyInfo blocks with curve_key_from_pem before mbedTLS. It already knows OID 1.3.101.110 and keeps the bytes as written. Add RFC 7748's unclamped Alice key to crypto_pem.fb's round trip.

Skeptic: I ran refute-keys/k5.fb on RFC 7748's Alice key as PKCS #8. Key.Bytes is 70076d0a...1db92c6a, a clamped copy. key.ToPem() == pem is False, and it writes ...UduSxq where the input had ...UduSwq. openssl pkey -text on the same PEM shows priv 77:07:6d:0a...2c:2a, as written. The public key is right either way (8520f009...9b4e6a), so exchanges still work. Only the bytes and the PEM round trip differ, which is why this is low.

## S21-keys-6 (low): pem.fb says ToPem writes a public RSA key in PKCS #1, but it writes SubjectPublicKeyInfo

Status: fixed in c8be79b, the comment, and 5d71f31, a check of the label, which passes before it too, since the code was right

Where: std/crypto/pem.fb:59

Key.ToPem's comment says 'a public one in SubjectPublicKeyInfo, but an RSA one in PKCS #1'. fb_crypto_key_to_pem calls mbedtls_pk_write_pubkey_pem, which writes '-----BEGIN PUBLIC KEY-----' (SubjectPublicKeyInfo) for every kind, RSA included. So a public key read from 'RSA PUBLIC KEY' comes back as 'PUBLIC KEY'. The ISRG Root X1 test only reads the PEM back, so it doesn't check the label. (Private RSA and P-256 keys do come out as PKCS #1 and SEC 1, as the comment says.)

```
// pem.fb's ToPem says a public RSA key is written in PKCS #1, as "RSA PUBLIC KEY".
Import std.core
Import std.crypto

Local pem = "-----BEGIN RSA PUBLIC KEY-----\n" +
    "MIIBCgKCAQEAoEYfoWyC7vXW0fI5PTqgkw9n7qn9XnNrr010FVHNHMWME3Mwyg48\n" +
    "+17SY5KGOGlEcXq+zQNfM97tv7/jmjj+oV4R4ed+n8NAFVjb/PUbbRW8Oy6gmgIP\n" +
    "vALtrWlAMITVhvKPvTQVbHBO1NpZFTdha43lyRBS0CnG951bGJxmLTYheNXRSL83\n" +
    "ziyR1p5cWaAgUYDr+zGUNjD3cSP6KR8nUYDORl7o9QSdlXyPwPEWEfaq1tscxLVa\n" +
    "iQld9nfcieS59tcbIsCHZWZ+GK9zxDim/Jc70D1dM/k5sRVTHq677pIa1zeOzsPz\n" +
    "fNQZyS2gBpNoeuSIRsRV1oKEVLxxt6wgYQIDAQAB\n" +
    "-----END RSA PUBLIC KEY-----\n"
Try
    Print(Key.FromPem(pem).ToPem())
Catch e As ParseError
    Print("ParseError {e.Kind}: {e.Message}")
End Try
```

Expected: The PEM as the comment describes it, PKCS #1, the same text as the input:
-----BEGIN RSA PUBLIC KEY-----
MIIBCgKCAQEAoEYfoWyC7vXW0fI5PTqgkw9n7qn9XnNrr010FVHNHMWME3Mwyg48
...
-----END RSA PUBLIC KEY-----

Actual: -----BEGIN PUBLIC KEY-----
MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAoEYfoWyC7vXW0fI5PTqg
kw9n7qn9XnNrr010FVHNHMWME3Mwyg48+17SY5KGOGlEcXq+zQNfM97tv7/jmjj+
oV4R4ed+n8NAFVjb/PUbbRW8Oy6gmgIPvALtrWlAMITVhvKPvTQVbHBO1NpZFTdh
a43lyRBS0CnG951bGJxmLTYheNXRSL83ziyR1p5cWaAgUYDr+zGUNjD3cSP6KR8n
UYDORl7o9QSdlXyPwPEWEfaq1tscxLVaiQld9nfcieS59tcbIsCHZWZ+GK9zxDim
/Jc70D1dM/k5sRVTHq677pIa1zeOzsPzfNQZyS2gBpNoeuSIRsRV1oKEVLxxt6wg
YQIDAQAB
-----END PUBLIC KEY-----

Suggested fix: Fix the comment: 'a public one, RSA's too, in SubjectPublicKeyInfo'. SPKI is the better default, and it's what OpenSSL writes. If PKCS #1 is really wanted, write Bytes (already RSAPublicKey DER) with pem_of(bytes, "RSA PUBLIC KEY"). Either way, add a check of the label.

Skeptic: I ran refute-keys/k6.fb, which reads an 'RSA PUBLIC KEY' PEM. It gives Kind RSA, IsPrivate False, and ToPem() writes '-----BEGIN PUBLIC KEY-----' (SubjectPublicKeyInfo, MIIBIjANBgkqhkiG9w0BAQEF...). pem.fb:59-60 says a public RSA key is written in PKCS #1. fb_crypto_key_to_pem calls mbedtls_pk_write_pubkey_pem for every public key. The comment is wrong and the behavior is reasonable, so it's a comment fix.

## S21-keys-7 (low): A damaged PEM key is reported as encrypted or of an unknown kind (WRONG_TYPE)

Status: fixed in c8be79b

Where: runtime/src/crypto_pem.cpp:223

When mbedTLS and curve_key_from_pem both fail, fb_crypto_key_from_pem returns unknown_key for any text containing '-----BEGIN ' and ' KEY-----'. Key.FromPem then throws ParseErrorKind.WRONG_TYPE with 'the PEM's key is encrypted, or of a kind std.crypto doesn't take'. A truncated or corrupt key of a kind std.crypto takes, like the RFC 8410 Ed25519 key cut short below, gets that message, which sends the user looking for a password or the wrong algorithm. FromPem's comment lists only no key, an encrypted key, and an unknown kind. A damaged CERTIFICATE block, by contrast, is a SYNTAX error.

```
// RFC 8410's Ed25519 private key with its last 18 bytes cut off: not encrypted, and of a kind
// std.crypto takes, but damaged.
Import std.core
Import std.crypto

Function Show(pem: String)
    Try
        Local key = Key.FromPem(pem)
        Print("{key.Kind}, private: {key.IsPrivate}")
    Catch e As ParseError
        Print("ParseError {e.Kind}: {e.Message}")
    End Try
End Function

Show("-----BEGIN PRIVATE KEY-----\n" +
    "MC4CAQAwBQYDK2VwBCIEINTuctv5E1hK1bbY8fdp\n" +
    "-----END PRIVATE KEY-----\n")
```

Expected: ParseError SYNTAX: (a message saying the PEM's key doesn't read)

Actual: ParseError WRONG_TYPE: the PEM's key is encrypted, or of a kind std.crypto doesn't take

Suggested fix: Keep unknown_key for 'ENCRYPTED PRIVATE KEY' or Proc-Type 4,ENCRYPTED blocks, MBEDTLS_ERR_PK_PASSWORD_REQUIRED, MBEDTLS_ERR_PK_UNKNOWN_PK_ALG, an unknown named curve, and keys pk_key refuses. Give a third code for a block whose base64 or DER doesn't read. Have FromPem throw SYNTAX with 'the PEM's key doesn't read' for it, and say so in FromPem's comment.

Skeptic: I ran refute-keys/k7.fb. RFC 8410's Ed25519 private key cut short, and a P-256 PKCS #8 key with its base64 cut, both give 'ParseError WRONG_TYPE: the PEM's key is encrypted, or of a kind std.crypto doesn't take'. A damaged CERTIFICATE gives SYNTAX. crypto_pem.cpp:223 turns any unread text containing '-----BEGIN ' and ' KEY-----' into unknown_key. FromPem's comment lists only no key, an encrypted key, or a kind with no KeyKind, so a damaged key gets a misleading kind and message. Nothing is wrong beyond the error report, so it's low.

## S21-keys-8 (low): crypto_signatures.fb says it checks RFC 7748's 5.2 X25519 vectors but checks only 6.1's

Status: fixed in d012bab, the vectors; the code was right, so they pass before it too

Where: tests/programs/crypto_signatures.fb:841

The section's comment reads "RFC 7748's X25519 vectors (5.2), and its Diffie-Hellman example (6.1)", but its four checks are all from 6.1 (Alice's and Bob's public keys and their shared secret). 5.2's two scalar/u-coordinate vectors are missing, and so are its iterated checks (1 and 1,000 iterations). The second vector's u has its top bit set, which X25519 must ignore. The program below runs them, and they pass: the outputs match RFC 7748 and an independent pure-Python RFC 7748 ladder. So the code is right and the test is short of what its comment and the done-when ('every algorithm matches its published test vectors') claim. Wycheproof covers the top-bit case, but not as RFC 7748's published vector.

```
// RFC 7748's 5.2 vectors: two scalar and u pairs, and the function iterated 1 and 1000 times.
Import std.core
Import std.collections
Import std.crypto

Function Unhex(text: String): UInt8[]
    Local bytes: UInt8[] = []
    Local i = 0
    Local t = text.Bytes
    While i + 1 < t.Count
        Local hi = t[i] As Int
        Local lo = t[i + 1] As Int
        hi = hi >= 97 ? hi - 87 : hi - 48
        lo = lo >= 97 ? lo - 87 : lo - 48
        bytes.Add((hi * 16 + lo) As UInt8)
        i += 2
    End While
    Return bytes
End Function

Function Hex(bytes: UInt8[]): String
    Local digits = ["0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "a", "b", "c", "d", "e", "f"]
    Local parts: String[] = []
    For b In bytes
        parts.Add(digits[(b >> 4) As Int] + digits[(b & 15) As Int])
    End For
    Return parts.Join("")
End Function

Print(Hex(X25519.SharedSecret(Unhex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4"), Unhex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c")) ?? []))
Print(Hex(X25519.SharedSecret(Unhex("4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d"), Unhex("e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493")) ?? []))
Local k = Unhex("0900000000000000000000000000000000000000000000000000000000000000")
Local u = k
For i = 1 To 1000
    Local next = X25519.SharedSecret(k, u) ?? []
    u = k
    k = next
    If i == 1 || i == 1000 Then Print("{i}: {Hex(k)}")
End For
```

Expected: These four lines, RFC 7748's 5.2 values, checked in crypto_signatures.fb:
c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552
95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957
1: 422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079
1000: 684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51

Actual: The program prints exactly the expected four lines, so X25519 is right. But crypto_signatures.fb has none of these checks ('RFC 7748: 4 checks', all from 6.1).

Suggested fix: Add the two 5.2 vectors and the 1- and 1,000-iteration checks to the RFC 7748 section of crypto_signatures.fb (and its .out count), or change the comment to say only 6.1.

Skeptic: crypto_signatures.fb:841 says "RFC 7748's X25519 vectors (5.2), and its Diffie-Hellman example (6.1)". Lines 842-845 are all 6.1 values (Alice's and Bob's public keys and their shared secret), and crypto_signatures.out has 'RFC 7748: 4 checks'. I ran refute-keys/k8.fb, which prints c3da5537...a28552, 95cbde94...ac7957, '1: 422c8e7a...ae3079', and '1000: 684cf59b...532c51'. Those are RFC 7748 5.2's values, so the code is right, but the test lacks the 5.2 checks its comment and the milestones row's done-when claim.

## S21-seal-2 (low): Bcrypt.Verify accepts crypt_blowfish's $2x$ hashes, where wrong non-ASCII passwords verify

Status: fixed in 7e2fbdd

Where: runtime/src/bcrypt.cpp:54

Verify's comment (std/crypto/passwords.fb:91) says it takes bcrypt's versions $2a$, $2b$ and $2y$, and that a hash that isn't bcrypt's is no password's. fb_bcrypt_verify hands any 60-character setting to _crypt_blowfish_rn, which also takes $2x$: the mode that reproduces the old sign-extension bug for bytes of 0x80 and up. Under $2x$, a byte with the high bit set wipes the bytes before it in its 32-bit key word. So different UTF-8 passwords, like "aё" and "bё", share one hash, and Verify gives True for a password the hash wasn't made from. That breaks the step's done-when, 'a wrong one doesn't' verify. The $2x$ hash below was made with the bundled crypt_blowfish.c (_crypt_blowfish_rn("a\xd1\x91", "$2x$04$CCCCCCCCCCCCCCCCCCCCC.")), and it is byte for byte the same hash as for "b\xd1\x91". The test's comment that crypt_blowfish's non-ASCII vectors are ones 'a String can't hold' misses "\xd1\x91" (ё), whose vector is a $2x$ one.

```
Import std.core
Import std.crypto

// crypt_blowfish's $2x$ hash of "aё", made by the bundled crypt_blowfish.c; "bё" gives the same.
Local kept = "$2x$04$CCCCCCCCCCCCCCCCCCCCC.eCrVmeIKP4Y.0WUE/pPRg1kiEEJb7s2"
Print(Bcrypt.Verify("aё", kept))
Print(Bcrypt.Verify("bё", kept))
// The $2b$ hash of "aё" tells them apart.
Print(Bcrypt.Verify("aё", "$2b$04$CCCCCCCCCCCCCCCCCCCCC.TmGY4ZIxKzKSs6E6l./3HxH.x2.MwqW"))
Print(Bcrypt.Verify("bё", "$2b$04$CCCCCCCCCCCCCCCCCCCCC.TmGY4ZIxKzKSs6E6l./3HxH.x2.MwqW"))
```

Expected: False
False
True
False
($2x$ isn't one of the versions Verify takes, so it is no password's hash, and a wrong password never verifies.)

Actual: True
True
True
False

Suggested fix: In fb_bcrypt_verify, return 0 unless expected starts with "$2a$", "$2b$" or "$2y$" (that is, refuse expected[2] == 'x'). Add the $2x$ vector above, and crypt_blowfish's "\xd1\x91" $2x$ vector, to crypto_passwords.fb as hashes that don't verify. If $2x$ is wanted for migrating old PHP hashes, say so in Verify's comment, with the collision it allows, instead.

Skeptic: I ran the finding's program, plus extra lines, in seal-verify/bx.fb, debug and release, with the same output both times. Against kept = $2x$04$CCC...eCrVmeIKP4Y.0WUE/pPRg1kiEEJb7s2, Verify("aё") is True, Verify("bё") is True, and Verify("cё") is True as well. The $2b$ hash takes "aё" (True) and refuses "bё" (False). crypt_blowfish's own $2x$ vector for "\xd1\x91" also verifies. fb_bcrypt_verify (runtime/src/bcrypt.cpp) checks only the length and NULs, then hands any setting to _crypt_blowfish_rn, which takes $2x$. Verify's comment (passwords.fb) says it takes '$2a$, $2b$, or $2y$' and that 'a hash that isn't bcrypt's is none'. So the code accepts a version its contract leaves out, and in that version different passwords verify. I rate it low, not medium. std's Hash only makes $2b$, so the collision needs a stored legacy $2x$ hash, and accepting those is crypt_blowfish's and PHP's intended compatibility mode. It is still a mismatch, fixed by refusing expected[2]=='x' or by documenting it. stdlib.md doesn't name the versions.

## Refuted by the skeptics (no action)

- S21-hash-4: Blake3.KeyedHash's data, Blake3.DeriveKey's material, and Hkdf's info take no String, though their siblings do. I reproduced the three compile errors (`expected UInt8[], found String`) with string_inputs.fb, but no documented behavior breaks. stdlib.md:398 says bytes go in 'as UInt8[], or a String's bytes', which `text.Bytes` satisfies, and that is the lane decision listed. It doesn't promise a String overload on every byte parameter. The code follows a consistent rule: the message or data being hashed, MACed, sealed, or signed gets a String overload, and the secondary byte inputs don't (keys, salts, Seal's `associated`, Hkdf's material and info). Hkdf's info sits with Seal's associated data and Hkdf's salt, and Blake3.DeriveKey's material sits with Hkdf's material. The only real outlier is Blake3.KeyedHash's data, since Hmac's data has a String overload, but leaving out a convenience overload doesn't contradict the docs. Adding one would be a nice API polish.
- S21-seal-3: crypto_passwords never checks the parameters Hash keeps, or a hash another implementation made. I ran the finding's program as seal-verify/pw.fb. It printed True five times: argon2's reference PHC string verifies, Argon2id.Hash("pw") starts with $argon2id$v=19$m=19456,t=2,p=1$, Bcrypt.Hash starts with $2b$12$, and pyca's $2b$ and $2y$ vectors verify. So the code does what stdlib.md says (19 MiB, 2 passes, 1 lane, cost 12). The done-when, 'every algorithm matches its published test vectors; a password hash verifies, and a wrong one doesn't', is met by crypto_passwords.fb: the RFC 9106 Derive vector, crypt_blowfish's $2a$ vectors, and right and wrong round trips. A test that doesn't pin the defaults is a coverage gap, not a behavior or spec contradiction, so I refute it. Adding the checks would still be cheap hardening.
- S21-seal-4: crypto_ciphers' ChaCha20-Poly1305 section says invalid vectors open to Null, but has none. seal-verify/cc.fb, debug and release, prints 114, then True for a flipped tag, a flipped ciphertext byte, other associated data, another key, and a truncated input. So ChaCha20Poly1305.Open refuses everything it should. The finding is right that crypto_ciphers.fb's ChaCha20-Poly1305 section has no invalid vectors: lines 219-458 have 120 Opens calls and no Null message, while the AES section has 27. The only ChaCha refusal checked is 'other cipher'. That makes the section's comment at line 218 ('an invalid one to Null') inaccurate, and is a coverage gap. No wrong behavior, and nothing contradicting the spec, stdlib.md, or the milestone row, so I refute it as a bug. The comment could be fixed, or invalid vectors added, as a minor cleanup.
