#!/bin/sh
# Makes the HTTP test server's certificates with OpenSSL 3.4 or later: a test CA, and a server
# certificate it signed for localhost, 127.0.0.1, and ::1, each valid from 2020 to 2120, so the
# tests never depend on the day they run. The CA's key isn't kept.
set -e
cd "$(dirname "$0")"
MSYS2_ARG_CONV_EXCL='*' openssl req -x509 -new -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes \
    -keyout ca.key -out test_ca.pem -subj "/CN=FutureBlitz Test CA" \
    -not_before 20200101000000Z -not_after 21200101000000Z \
    -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign"
MSYS2_ARG_CONV_EXCL='*' openssl req -new -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes \
    -keyout server_key.pem -out server.csr -subj "/CN=localhost"
printf '%s\n' "basicConstraints=critical,CA:FALSE" "keyUsage=critical,digitalSignature" \
    "extendedKeyUsage=serverAuth" "subjectAltName=DNS:localhost,IP:127.0.0.1,IP:::1" > server.ext
openssl x509 -req -in server.csr -CA test_ca.pem -CAkey ca.key -out server.pem \
    -not_before 20200101000000Z -not_after 21200101000000Z -extfile server.ext
rm ca.key server.csr server.ext
