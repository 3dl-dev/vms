#!/bin/sh
# run_ssh_build.sh — acceptance for the OpenSSH CLIENTS port, ssh vendoring +
# static-musl build (item vms-22a, parent vms-843,
# docs/design-openssh-port-ovmx.md).
#
# Runs build-openssh.sh, then asserts:
#   1. build-openssh.sh fetches the SHA256-pinned OpenSSH tarball and builds the
#      `ssh` client FULLY STATIC against musl + the vendored libcrypto
#      (third-party/libcrypto) — the make-or-break: OpenSSH+LibreSSL+musl links.
#   2. `ssh` is a fully static ELF (no shared-lib NEEDED entries).
#   3. `ssh -V` runs and prints an OpenSSH_10.0 version banner (the binary is
#      genuinely executable, not just linked).
#
# A pass = the ssh client is vendored and cross-builds static. The OVMX transport
# is NOT part of this build: ssh uses STANDARD BSD sockets, which OVMX supplies
# via the BSD-sockets RTL veneer over BGn: (a separate prerequisite — see the
# design doc). Building ssh as the OVMX IMGACT image over that veneer + the
# real-KEX QEMU proof are the follow-on integration.
#
# Run inside the project's musl container (alpine:3.20) — NEVER on the bare host
# (shared-host rule). CI job: openssh-static-musl.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)           # third-party/openssh/test
DIR=$(cd "$HERE/.." && pwd)                    # third-party/openssh
CC="${CC:-gcc}"
WORK="${WORK:-/tmp/ovmx-openssh}"
export WORK CC

echo "== build the vendored static ssh =="
BUILD_OUT=$(sh "$DIR/build-openssh.sh")
echo "$BUILD_OUT"
SSH=$(echo "$BUILD_OUT" | sed -n 's/^OVMX_SSH_BIN=//p' | tail -1)
[ -x "$SSH" ] || { echo "FAIL: ssh binary path not reported by build-openssh.sh"; exit 1; }

echo
echo "== confirm ssh is fully static (no shared-lib NEEDED entries) =="
if command -v readelf >/dev/null 2>&1; then
    if readelf -d "$SSH" 2>/dev/null | grep -q '(NEEDED)'; then
        echo "FAIL: ssh has shared-library NEEDED entries — not static:"
        readelf -d "$SSH" | grep '(NEEDED)'
        exit 1
    fi
    echo "   readelf -d: no (NEEDED) entries — fully static"
fi

echo
echo "== run ssh -V (version banner) =="
set +e
VOUT=$("$SSH" -V 2>&1)
RC=$?
set -e
echo "   $VOUT"
[ "$RC" -eq 0 ] || { echo "FAIL: ssh -V exited $RC"; exit 1; }
echo "$VOUT" | grep -q "OpenSSH_10.0" \
    || { echo "FAIL: expected OpenSSH_10.0 version banner"; exit 1; }

echo
echo "DONE: ssh vendored + cross-builds static-musl against libcrypto."
