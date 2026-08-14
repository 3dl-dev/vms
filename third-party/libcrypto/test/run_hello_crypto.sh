#!/bin/sh
# run_hello_crypto.sh — P-C acceptance (bead vms-2e8, parent vms-843).
#
# Builds the vendored static libcrypto.a (musl, out-of-tree) via
# ../build-libcrypto.sh, then compiles hello-crypto.c, STATICALLY links it
# against that libcrypto.a, RUNS it, and asserts the SHA-256 known-answer (and a
# bignum op) pass. A pass = the OpenSSH port can link crypto against this lib for
# the musl-static bootable image, with NO dependency on the post-1.0
# OpenSSL-as-OVMX-image (vms-4fa).
#
# DONE conditions (all proven by a REAL run, nothing mocked):
#   1. build-libcrypto.sh produces libcrypto.a from the SHA256-pinned tarball.
#   2. hello-crypto links STATICALLY against libcrypto.a (`gcc -static`), no
#      shared libcrypto, no ldd dynamic-libcrypto dependency.
#   3. the program runs and prints "PASS" (SHA-256("abc") KAT + BN_mul KAT).
#
# Run inside the project's musl container (alpine:3.20) — the toolchain
# distro/Dockerfile.bootable / the OVMX_STATIC path use — NEVER on the bare host
# (shared-host rule). CI job: libcrypto-static-musl.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)           # third-party/libcrypto/test
DIR=$(cd "$HERE/.." && pwd)                    # third-party/libcrypto
CC="${CC:-gcc}"
WORK="${WORK:-/tmp/ovmx-libcrypto}"
export WORK CC

echo "== build the vendored static libcrypto =="
BUILD_OUT=$(sh "$DIR/build-libcrypto.sh")
echo "$BUILD_OUT"
LIB=$(echo "$BUILD_OUT" | sed -n 's/^OVMX_LIBCRYPTO_A=//p' | tail -1)
INC=$(echo "$BUILD_OUT" | sed -n 's/^OVMX_LIBCRYPTO_INCLUDE=//p' | tail -1)
[ -f "$LIB" ] || { echo "FAIL: libcrypto.a path not reported by build-libcrypto.sh"; exit 1; }
[ -d "$INC" ] || { echo "FAIL: include dir not reported by build-libcrypto.sh"; exit 1; }

BIN="$WORK/hello-crypto"
echo
echo "== compile + STATIC-link hello-crypto against libcrypto.a =="
# -static: no dynamic libcrypto anywhere — this is the musl-static bootable link.
"$CC" -static -O2 -Wall -Wextra -I"$INC" \
    -o "$BIN" "$HERE/hello-crypto.c" "$LIB"
[ -x "$BIN" ] || { echo "FAIL: hello-crypto not produced at $BIN"; exit 1; }

echo
echo "== confirm the binary is fully static (no shared-lib NEEDED entries) =="
# A -static ELF has NO dynamic section / no (NEEDED) entries at all. readelf is
# path-name-safe (ldd's message echoes the binary path, which contains the
# substring 'libcrypto' via the WORK dir — do not grep that).
if command -v readelf >/dev/null 2>&1; then
    if readelf -d "$BIN" 2>/dev/null | grep -q '(NEEDED)'; then
        echo "FAIL: hello-crypto has shared-library NEEDED entries — not static:"
        readelf -d "$BIN" | grep '(NEEDED)'
        exit 1
    fi
    echo "   readelf -d: no (NEEDED) entries — fully static"
fi

echo
echo "== run hello-crypto (known-answer acceptance) =="
set +e
"$BIN" > "$WORK/hello-crypto.out" 2>&1
RC=$?
set -e
sed 's/^/   /' "$WORK/hello-crypto.out"
echo "exit code = $RC"
[ "$RC" -eq 0 ] || { echo "FAIL: hello-crypto exited $RC"; exit 1; }
grep -q "PASS: static libcrypto is linkable and correct" "$WORK/hello-crypto.out" \
    || { echo "FAIL: PASS marker not found"; exit 1; }

echo
echo "DONE: vendored static libcrypto is linkable + correct (SHA-256 + bignum KAT)."
