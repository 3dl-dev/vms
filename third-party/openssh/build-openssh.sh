#!/bin/sh
# build-openssh.sh — fetch the pinned OpenSSH-portable tarball (SHA256-verified)
# and cross-build the STATIC `ssh` client against musl + the vendored static
# libcrypto (third-party/libcrypto), entirely out-of-tree.
#
# OpenSSH CLIENTS port (item vms-22a, parent vms-843,
# docs/design-openssh-port-ovmx.md). Vendoring model = PINNED FETCH, same as
# third-party/libcrypto (see VENDOR-REV for the pin, license, and the p2/p1
# mirror note). The tarball is verified against a pinned SHA256 (reproducible;
# mismatch = hard fail) and built to $WORK (default /tmp) — NOTHING is installed
# on the host and no product lands in the repo. Run inside the project's musl
# container (alpine:3.20), NEVER on the bare host (shared-host rule).
#
# This proves the make-or-break for the whole port: OpenSSH + LibreSSL + musl
# links a fully-static `ssh`. The OVMX TRANSPORT does NOT live in this build:
# ssh reaches TCP through STANDARD BSD sockets, and OVMX supplies those via the
# BSD-sockets RTL veneer over BGn: (a separate prerequisite; see the design doc)
# — the app never speaks $QIO/TCPIP$DEVICE:. Building ssh as the OVMX IMGACT
# image linked against that veneer is the follow-on integration.
#
# Usage:
#   sh build-openssh.sh              # static ssh build
#   WORK=/tmp/foo sh build-openssh.sh
#
# On success prints, on the LAST lines, the paths downstream consumers need:
#   OVMX_SSH_BIN=<abs path to the static ssh binary>
#   OVMX_SSH_SRCDIR=<abs path to the extracted OpenSSH source tree>
set -eu

OSSH_VERSION="${OSSH_VERSION:-10.0p1}"
OSSH_SHA256="${OSSH_SHA256:-021a2e709a0edf4250b1256bd5a9e500411a90dddabea830ed59cef90eb9d85c}"
OSSH_INNER="${OSSH_INNER:-openssh-10.0p1}"
TARBALL="openssh-${OSSH_VERSION}.tar.gz"
URL_PRIMARY="${OVMX_OPENSSH_URL:-https://cdn.openbsd.org/pub/OpenBSD/OpenSSH/portable/${TARBALL}}"
URL_MIRROR="https://ftp.openbsd.org/pub/OpenBSD/OpenSSH/portable/${TARBALL}"

WORK="${WORK:-/tmp/ovmx-openssh}"
CC="${CC:-gcc}"

HERE="$(cd "$(dirname "$0")" && pwd)"        # third-party/openssh
ROOT="$(cd "$HERE/../.." && pwd)"            # repo root

mkdir -p "$WORK"
cd "$WORK"

fetch() {
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
echo "${OSSH_SHA256}  ${TARBALL}" | sha256sum -c - || {
    echo "FAIL: SHA256 mismatch for ${TARBALL} — refusing to build an unpinned tree" >&2
    echo "  expected ${OSSH_SHA256}" >&2
    echo "  got      $(sha256sum "${TARBALL}" | awk '{print $1}')" >&2
    exit 1
}

# ---- vendored static libcrypto (reuse the libcrypto build) ----------------
echo "== build the vendored static libcrypto =="
LC_OUT="$(sh "$ROOT/third-party/libcrypto/build-libcrypto.sh")"
echo "$LC_OUT" | sed -n 's/^OVMX_LIBCRYPTO/  &/p'
LIBCRYPTO_A="$(echo "$LC_OUT" | sed -n 's/^OVMX_LIBCRYPTO_A=//p' | tail -1)"
LIBCRYPTO_INC="$(echo "$LC_OUT" | sed -n 's/^OVMX_LIBCRYPTO_INCLUDE=//p' | tail -1)"
[ -f "$LIBCRYPTO_A" ] || { echo "FAIL: libcrypto.a not reported by build-libcrypto.sh" >&2; exit 1; }

# Stage an --with-ssl-dir prefix OpenSSH's configure understands (lib/ + include/).
SSL="$WORK/ssl"
rm -rf "$SSL"; mkdir -p "$SSL/lib" "$SSL/include"
cp "$LIBCRYPTO_A" "$SSL/lib/"
cp -r "$LIBCRYPTO_INC/openssl" "$SSL/include/"

# ---- extract + configure + build the static ssh client --------------------
SRCDIR="$WORK/${OSSH_INNER}"
rm -rf "$SRCDIR"
echo "== extract =="
tar xzf "$TARBALL"
[ -d "$SRCDIR" ] || { echo "FAIL: expected inner dir $SRCDIR not found after extract" >&2; exit 1; }

cd "$SRCDIR"
echo "== configure (static, musl, vendored libcrypto) =="
CC="$CC" CFLAGS="-O2" LDFLAGS="-static" \
  ./configure --host=x86_64-linux-musl \
    --with-ssl-dir="$SSL" \
    --without-zlib \
    --without-pam \
    --without-selinux \
    --disable-strip \
    >"$WORK/openssh-configure.log" 2>&1 || {
        echo "FAIL: configure failed; tail of log:" >&2
        tail -40 "$WORK/openssh-configure.log" >&2
        exit 1
    }

echo "== make ssh (static) =="
make -j"$(nproc 2>/dev/null || echo 2)" ssh >"$WORK/openssh-make.log" 2>&1 || {
    echo "FAIL: make ssh failed; tail of log:" >&2
    tail -60 "$WORK/openssh-make.log" >&2
    exit 1
}
[ -x "$SRCDIR/ssh" ] || { echo "FAIL: ssh binary not produced" >&2; exit 1; }

echo "== built ssh =="
ls -l "$SRCDIR/ssh"
echo "OVMX_SSH_BIN=$SRCDIR/ssh"
echo "OVMX_SSH_SRCDIR=$SRCDIR"
