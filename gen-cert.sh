#!/bin/bash

# Generate some test certificates which are used by the regression test suite:
#
#   tls/ca.{crt,key}          Self signed CA certificate.
#   tls/atophttpd.{crt,key}   A certificate for both server and client usage.
#   tls/client.{crt,key}      A certificate restricted for SSL client usage.
#   tls/server.{crt,key}      A certificate restricted for SSL server usage.
#   tls/atophttpd.dh          DH Params file.

generate_cert() {
    local name=$1
    local cn=$2

    local keyfile=tls/${name}.key
    local certfile=tls/${name}.crt

    [ -f $keyfile ] || openssl genrsa -out $keyfile 2048
    openssl req \
        -new -sha256 \
        -subj "/O=Atophttpd Test/CN=$cn" \
        -key $keyfile | \
        openssl x509 \
            -req -sha256 \
            -CA tls/ca.crt \
            -CAkey tls/ca.key \
            -CAserial tls/ca.txt \
            -CAcreateserial \
            -days 365 \
            -extfile tls/openssl.cnf \
            -extensions "$name" \
            -out $certfile
}

read -p "Enter DNS name: " DNS
read -p "Enter IP address: " IP

SAN="DNS:localhost,IP:127.0.0.1"
if [ ! -z "$DNS" ]; then
    SAN="$SAN,DNS:$DNS"
fi

if [ ! -z "$IP" ]; then
    SAN="$SAN,IP:$IP"
fi

mkdir -p tls
[ -f tls/ca.key ] || openssl genrsa -out tls/ca.key 4096
openssl req \
    -x509 -new -nodes -sha256 \
    -key tls/ca.key \
    -days 3650 \
    -subj '/O=Atophttpd Test/CN=Certificate Authority' \
    -addext 'basicConstraints=critical,CA:TRUE' \
    -addext 'keyUsage=critical,keyCertSign,cRLSign' \
    -out tls/ca.crt

cat > tls/openssl.cnf <<_END_
[ server ]
basicConstraints = critical,CA:FALSE
keyUsage = critical,digitalSignature,keyEncipherment
extendedKeyUsage = serverAuth
subjectAltName = ${SAN}

[ client ]
basicConstraints = critical,CA:FALSE
keyUsage = critical,digitalSignature
extendedKeyUsage = clientAuth

[ atophttpd ]
basicConstraints = critical,CA:FALSE
keyUsage = critical,digitalSignature,keyEncipherment
extendedKeyUsage = serverAuth,clientAuth
subjectAltName = ${SAN}
_END_

generate_cert server "Server-only"
generate_cert client "Client-only"
generate_cert atophttpd "Generic-cert"

[ -f tls/atophttpd.dh ] || openssl dhparam -out tls/atophttpd.dh 2048
