#!/bin/sh
# build-libcrypto.sh — fetch the pinned LibreSSL tarball (SHA256-verified) and
# cross-build a STATIC libcrypto.a against musl, entirely out-of-tree.
#
# OpenSSH port prereq P-C (bead vms-2e8, parent vms-843,
# docs/design-openssh-port-ovmx.md §3.1): the crypto foundation the OpenSSH
# clients (vms-22a) and the sshd swap (vms-0cd) link against for the musl-static
# bootable image — INDEPENDENT of the post-1.0 OpenSSL-as-OVMX-image (vms-4fa).
#
# Vendoring model: PINNED FETCH, not an in-tree source copy (see VENDOR-REV for
# the pin + license + LibreSSL-vs-OpenSSL rationale). The tarball is verified
# against a pinned SHA256 (reproducible; mismatch = hard fail) and built to
# $WORK (default /tmp) — NOTHING is installed on the host and no product lands
# in the repo. Run inside the project's musl container (alpine:3.20), the same
# toolchain distro/Dockerfile.bootable's link-native stage / the OVMX_STATIC
# musl path use — NEVER on the bare host (shared-host rule).
#
# Usage:
#   sh build-libcrypto.sh              # fetch+build into /tmp/ovmx-libcrypto
#   WORK=/tmp/foo sh build-libcrypto.sh
#
# On success prints, on the LAST line, the two paths downstream consumers need:
#   OVMX_LIBCRYPTO_A=<abs path to libcrypto.a>
#   OVMX_LIBCRYPTO_INCLUDE=<abs path to the openssl/ header include dir>
set -eu

LIBRESSL_VERSION="${LIBRESSL_VERSION:-4.1.2}"
LIBRESSL_SHA256="${LIBRESSL_SHA256:-fba4e2fa2a7f52306df7a389970a10e98b97eb0edb299a9fdb9dbf49999c61e1}"
TARBALL="libressl-${LIBRESSL_VERSION}.tar.gz"
# Primary + mirror. SHA256 verification below makes the source untrusted-safe.
URL_PRIMARY="${OVMX_LIBRESSL_URL:-https://ftp.openbsd.org/pub/OpenBSD/LibreSSL/${TARBALL}}"
URL_MIRROR="https://cdn.openbsd.org/pub/OpenBSD/LibreSSL/${TARBALL}"

WORK="${WORK:-/tmp/ovmx-libcrypto}"
CC="${CC:-gcc}"

mkdir -p "$WORK"
cd "$WORK"

fetch() {
    # $1 = url, $2 = out
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL -o "$2" "$1"
    elif command -v wget >/dev/null 2>&1; then
        wget -q -O "$2" "$1"
    else
        echo "FAIL: neither curl nor wget available to fetch $1" >&2
        return 1
    fi
}

if [ ! -f "$TARBALL" ]; then
    echo "== fetch $TARBALL =="
    fetch "$URL_PRIMARY" "$TARBALL" || fetch "$URL_MIRROR" "$TARBALL" || {
        echo "FAIL: could not fetch $TARBALL from primary or mirror" >&2
        exit 1
    }
fi

echo "== verify SHA256 (pinned, reproducible) =="
echo "${LIBRESSL_SHA256}  ${TARBALL}" | sha256sum -c - || {
    echo "FAIL: SHA256 mismatch for ${TARBALL} — refusing to build an unpinned tree" >&2
    echo "  expected ${LIBRESSL_SHA256}" >&2
    echo "  got      $(sha256sum "${TARBALL}" | awk '{print $1}')" >&2
    exit 1
}

SRC="$WORK/libressl-${LIBRESSL_VERSION}"
if [ ! -d "$SRC" ]; then
    echo "== extract =="
    tar xzf "$TARBALL"
fi

if [ ! -f "$SRC/crypto/.libs/libcrypto.a" ]; then
    echo "== configure (static libcrypto, no shared, no tests) =="
    ( cd "$SRC" && CC="$CC" ./configure \
        --enable-static --disable-shared --disable-tests >/dev/null )
    echo "== build libcrypto.a (musl-static) =="
    # Only libcrypto — OpenSSH links libcrypto, not libssl/libtls. Keeps the
    # build lean and matches this bead's exact scope.
    make -C "$SRC/crypto" -j"$(nproc 2>/dev/null || echo 2)" libcrypto.la >/dev/null
fi

LIB="$SRC/crypto/.libs/libcrypto.a"
INC="$SRC/include"
[ -f "$LIB" ] || { echo "FAIL: libcrypto.a not produced at $LIB" >&2; exit 1; }
[ -f "$INC/openssl/sha.h" ] || { echo "FAIL: headers missing at $INC/openssl" >&2; exit 1; }

echo "== built libcrypto.a =="
ls -l "$LIB"
echo "OVMX_LIBCRYPTO_A=$LIB"
echo "OVMX_LIBCRYPTO_INCLUDE=$INC"
