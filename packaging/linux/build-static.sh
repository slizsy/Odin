#!/bin/sh
# builds a fully static linux binary inside alpine (musl), so it runs on any distro.
# ci runs it like: docker run --rm -v "$PWD":/src -w /src alpine:3.20 sh packaging/linux/build-static.sh
set -eu

apk add --no-cache build-base pkgconf python3 file \
    zlib-dev zlib-static curl-dev curl-static \
    openssl-dev openssl-libs-static nghttp2-dev nghttp2-static \
    brotli-dev brotli-static zstd-dev zstd-static \
    libidn2-dev libidn2-static libunistring-dev libunistring-static \
    libpsl-dev libpsl-static c-ares-dev c-ares-static >/dev/null

make clean
make STATIC=1 CFLAGS=-O2
strip odin
file odin

# prove it actually works before anyone downloads it
./odin version
make test STATIC=1 SAN= CFLAGS=-O2
