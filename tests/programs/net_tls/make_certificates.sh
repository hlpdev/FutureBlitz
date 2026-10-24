#!/bin/sh
# Makes the TLS tests' certificates with OpenSSL 3.4 or later, into certificates.fb beside this
# script: a test CA, and what it signed: a server certificate for localhost, 127.0.0.1, and ::1,
# one with an RSA key, one with an Ed25519 key, which mbedTLS doesn't read, one for another host,
# one that expired in 2021, and a client certificate; and a certificate for localhost that a CA
# no test trusts signed. Each but the expired one is valid from 2020 to 2120, so the tests never
# depend on the day they run. The CAs' keys aren't kept.
set -e
cd "$(dirname "$0")"
# A folder of its own, by a relative path, which the native OpenSSL on Windows reads too.
work=.certificates
rm -rf "$work"
mkdir "$work"
trap 'rm -rf "$work"' EXIT

# A CA, $1, named $2.
ca() {
    MSYS2_ARG_CONV_EXCL='*' openssl req -x509 -new -newkey ec -pkeyopt ec_paramgen_curve:P-256 \
        -nodes -keyout "$work/$1.key" -out "$work/$1.pem" -subj "/CN=$2" \
        -not_before 20200101000000Z -not_after 21200101000000Z \
        -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign"
}

# A certificate, $1, for $2, which the CA $3 signs, with the extensions $4, valid from $5 to $6,
# over a key of the algorithm $7.
certificate() {
    if [ "$7" = rsa ]; then
        openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out "$work/$1.key" 2>/dev/null
    elif [ "$7" = ed25519 ]; then
        openssl genpkey -algorithm ED25519 -out "$work/$1.key"
    else
        openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out "$work/$1.key"
    fi
    MSYS2_ARG_CONV_EXCL='*' openssl req -new -key "$work/$1.key" -out "$work/$1.csr" -subj "/CN=$2"
    printf '%s\n' "basicConstraints=critical,CA:FALSE" "keyUsage=critical,digitalSignature,keyEncipherment" \
        "$4" > "$work/$1.ext"
    openssl x509 -req -in "$work/$1.csr" -CA "$work/$3.pem" -CAkey "$work/$3.key" \
        -out "$work/$1.pem" -not_before "$5" -not_after "$6" -extfile "$work/$1.ext" 2>/dev/null
}

server="extendedKeyUsage=serverAuth
subjectAltName=DNS:localhost,IP:127.0.0.1,IP:::1"
ca ca "FutureBlitz Test CA"
ca rogue_ca "FutureBlitz Rogue CA"
certificate localhost localhost ca "$server" 20200101000000Z 21200101000000Z ec
certificate rsa localhost ca "$server" 20200101000000Z 21200101000000Z rsa
certificate ed25519 localhost ca "$server" 20200101000000Z 21200101000000Z ed25519
certificate other other.example ca "extendedKeyUsage=serverAuth
subjectAltName=DNS:other.example" 20200101000000Z 21200101000000Z ec
certificate expired localhost ca "$server" 20200101000000Z 20210101000000Z ec
certificate client "FutureBlitz Test Client" ca "extendedKeyUsage=clientAuth" \
    20200101000000Z 21200101000000Z ec
certificate rogue localhost rogue_ca "$server" 20200101000000Z 21200101000000Z ec

# Each as a Const of certificates.fb, a certificate's PEM and its key's, which net_tls_tcp's
# folder has too, since a test program reads only its own.
{
    echo "// Made by make_certificates.sh with OpenSSL: the TLS tests' certificates and keys, as PEM."
    echo "// Make them again with that script."
    for name in ca rogue_ca localhost rsa ed25519 other expired client rogue; do
        echo
        echo "Public Const ${name}_pem = \"\"\""
        sed 's/^/    /' "$work/$name.pem"
        echo "    \"\"\""
        if [ -f "$work/$name.csr" ]; then
            echo
            echo "Public Const ${name}_key = \"\"\""
            openssl pkey -in "$work/$name.key" | sed 's/^/    /'
            echo "    \"\"\""
        fi
    done
} > certificates.fb
mkdir -p ../net_tls_tcp
cp certificates.fb ../net_tls_tcp/certificates.fb
