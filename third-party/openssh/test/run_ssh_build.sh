#!/bin/sh
# run_ssh_build.sh — acceptance for the OpenSSH CLIENTS port, ssh increment
# (item vms-22a, parent vms-843, docs/design-openssh-port-ovmx.md §2.3).
#
# Runs build-openssh.sh with the OVMX BGn: transport substitution enabled, then
# asserts:
#   1. build-openssh.sh fetches the SHA256-pinned OpenSSH tarball and builds the
#      `ssh` client FULLY STATIC against musl + the vendored libcrypto
#      (third-party/libcrypto) — the make-or-break: OpenSSH+LibreSSL+musl links.
#   2. `ssh` is a fully static ELF (no shared-lib NEEDED entries).
#   3. `ssh -V` runs and prints an OpenSSH_10.0 version banner (the binary is
#      genuinely executable, not just linked).
#   4. (build-openssh.sh's own OVMX transport compile-check) the shim TU compiles
#      clean against the real OVMX headers and the patched sshconnect.c compiles
#      with the OVMX branch active, its object referencing ovmx_bg_connect.
#
# A pass = the ssh client is vendored, cross-builds static, and the BGn:
# transport substitution is wired + type-correct against the executive service
# surface. The full IMGACT-image link (against libvms) + the real-KEX QEMU proof
# over BGn: are the continuation (see VENDOR-REV / the design doc).
#
# Run inside the project's musl container (alpine:3.20) — NEVER on the bare host
# (shared-host rule). CI job: openssh-static-musl.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)           # third-party/openssh/test
DIR=$(cd "$HERE/.." && pwd)                    # third-party/openssh
CC="${CC:-gcc}"
WORK="${WORK:-/tmp/ovmx-openssh}"
export WORK CC

echo "== build the vendored static ssh + OVMX transport compile-check =="
BUILD_OUT=$(OVMX_BG_TRANSPORT=1 sh "$DIR/build-openssh.sh")
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
echo "DONE: ssh vendored + cross-builds static-musl against libcrypto; OVMX BGn:"
echo "      transport substitution compiles + is wired (ovmx_bg_connect)."
