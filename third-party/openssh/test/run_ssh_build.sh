#!/bin/sh
# run_ssh_build.sh — acceptance for the OpenSSH `ssh` client on the OVMX
# BSD-sockets veneer (item vms-22a, parent vms-843,
# docs/design-openssh-port-ovmx.md).
#
# Builds `ssh` from the SHA256-pinned OpenSSH tarball with the OVMX transport +
# poll substitution (OVMX_VENEER=1): the vendored ssh keeps its STANDARD BSD
# calls, and they resolve to the OVMX veneer over BGn: (src/vmstcpip/sockets +
# the vms_kif_bg_* executive path) -- NO $QIO/vms_kif code in OpenSSH, NO raw
# Linux socket(). Then asserts:
#   1. `ssh` cross-builds FULLY STATIC against musl + the vendored libcrypto AND
#      the OVMX veneer object set (all veneer/executive symbols resolve).
#   2. the substitution is wired into the binary: ovmx_ssh_connect (transport),
#      ovmx_ssh_read/ovmx_ssh_write/ovmx_ssh_sshbuf_read (packet I/O), the veneer
#      (ovmx_socket/connect/send/recv/pollfd) and the executive readiness-fd
#      entry (vms_kif_bg_pollfd) are all present.
#   3. `ssh -V` runs and prints an OpenSSH_10.0 banner.
#
# A pass = the ssh client is vendored and its transport + event-loop poll are
# substituted onto the veneer, linked static-musl. The real KEX handshake over a
# live /dev/vms is the QEMU Kernel-Executive proof (see the design doc §7).
#
# Run inside the project's musl container (alpine:3.20) — NEVER on the bare host.
# CI job: openssh-static-musl.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)           # third-party/openssh/test
DIR=$(cd "$HERE/.." && pwd)                    # third-party/openssh
CC="${CC:-gcc}"
WORK="${WORK:-/tmp/ovmx-openssh}"
export WORK CC

echo "== build the vendored ssh with the OVMX veneer transport+poll substitution =="
BUILD_OUT=$(OVMX_VENEER=1 sh "$DIR/build-openssh.sh")
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
echo "== confirm the --wrap de-veneer dispatch is wired into the binary =="
# vms-9ac full de-veneer: the OpenSSH source is UNMODIFIED, so there are NO
# ovmx_ssh_* shim symbols any more. What must be linked is the __wrap_* dispatch
# (socket/connect included -- no sshconnect source patch) over the veneer's
# ovmx_*/$QIO transport. No AF_UNIX socketpair, no glue.
if command -v nm >/dev/null 2>&1; then
    for sym in __wrap_socket __wrap_connect __wrap_read __wrap_write \
               ovmx_socket ovmx_connect ovmx_send ovmx_recv ovmx_pollfd \
               vms_kif_bg_pollfd; do
        nm "$SSH" | grep -q " T $sym" || {
            echo "FAIL: expected symbol '$sym' not linked into ssh — --wrap dispatch not wired"
            exit 1
        }
    done
    echo "   nm: --wrap dispatch + veneer + executive readiness-fd symbols all present"
    # And prove the fabrication is GONE: no ovmx_ssh_* glue shim may survive.
    for gone in ovmx_ssh_connect ovmx_ssh_read ovmx_ssh_write ovmx_ssh_sshbuf_read; do
        nm "$SSH" | grep -q " T $gone" && {
            echo "FAIL: retired glue symbol '$gone' still linked — the socketpair pump was not excised"
            exit 1
        }
    done
    echo "   nm: no retired ovmx_ssh_* glue shim survives (socketpair pump excised)"
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
echo "DONE: ssh vendored + transport/event-loop substituted onto the OVMX veneer,"
echo "      cross-built static-musl with all veneer/executive symbols resolved."
