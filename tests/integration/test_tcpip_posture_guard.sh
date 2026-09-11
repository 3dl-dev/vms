#!/bin/bash
# test_tcpip_posture_guard.sh (rd vms-21b) -- POSTURE GUARD for the shipped image.
#
# The shipped OVMX distribution must auto-start NO network service out of the box:
# enabling a network-facing service by default (SSH accepting default SYSTEM/MANAGER
# creds, or even a daytime listener on :13) is a SECURITY-POSTURE decision reserved
# to the operator (Baron), NOT something that lands silently in a code change. This
# test makes that posture a durable INVARIANT: it REDS if a future change either
#   (a) wires @SYS$STARTUP:TCPIP$STARTUP into the shipped SYSTARTUP_VMS.COM (which
#       would start the aux server at boot), OR
#   (b) ships an ENABLED service line in the shipped TCPIP$SERVICE.DAT (which the
#       aux server would then bind),
# forcing that enable-by-default decision to be made explicitly (and this guard
# updated deliberately) rather than drifting in.
#
# It asserts BEHAVIOR-NEUTRALITY BY CONSTRUCTION for vms-21b: the shipped base
# image carries NO aux-server image at all (TCP/IP Services is a layered product;
# TCPIP$INETD.EXE/TCPIP$DAYTIME.EXE are staged only by the --build-arg
# OVMX_TEST_ENABLE_TCPIP=1 test overlay, never the shipped rootfs), and even the
# RUN/DETACHED launch path stays inert because neither trigger above is present in
# the shipped SYSTARTUP_VMS.COM / TCPIP$SERVICE.DAT. The daytime cold-boot proof
# enables TCP/IP only via that TEST overlay (distro/rootfs-test-tcpip/), never the
# shipped rootfs. Cheap (grep-level, no boot).
set -u
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SYSMGR="$REPO_ROOT/distro/rootfs/vms/SYS0/SYSCOMMON/SYSMGR"
SYSEXE="$REPO_ROOT/distro/rootfs/vms/SYS0/SYSCOMMON/SYSEXE"
SYSTARTUP="$SYSMGR/SYSTARTUP_VMS.COM"
SERVICE_DAT="$SYSEXE/TCPIP\$SERVICE.DAT"
FAIL=0
ok(){  echo "  PASS: $*"; }
bad(){ echo "  FAIL: $*"; FAIL=1; }

echo "=== test_tcpip_posture_guard (shipped default auto-starts no network service, rd vms-21b/vms-843a) ==="

[ -f "$SYSTARTUP" ] || { echo "FATAL: shipped SYSTARTUP_VMS.COM not found at $SYSTARTUP"; exit 1; }
[ -f "$SERVICE_DAT" ] || { echo "FATAL: shipped TCPIP\$SERVICE.DAT not found at $SERVICE_DAT"; exit 1; }

# (a) shipped SYSTARTUP must NOT invoke the TCP/IP aux-server startup. A DCL
# invocation is a non-comment line ('$' in col 1, not '$!') naming TCPIP$STARTUP.
if grep -nE '^\$[^!].*TCPIP\$STARTUP' "$SYSTARTUP" >/dev/null 2>&1; then
    bad "shipped SYSTARTUP_VMS.COM INVOKES @SYS\$STARTUP:TCPIP\$STARTUP -- that auto-starts the aux server at boot (operator posture call, Baron-reserved). If enabling TCP/IP by default is intended, make it explicit and update this guard."
    grep -nE '^\$[^!].*TCPIP\$STARTUP' "$SYSTARTUP" | sed 's/^/      /'
else
    ok "shipped SYSTARTUP_VMS.COM does NOT invoke TCPIP\$STARTUP (aux server not auto-started)"
fi

# (b) shipped SERVICE.DAT must have NO enabled service lines. Enabled == a
# non-comment (not '!'), non-blank line -- the aux server binds exactly those.
ENABLED=$(grep -vE '^[[:space:]]*!|^[[:space:]]*$' "$SERVICE_DAT" || true)
if [ -n "$ENABLED" ]; then
    bad "shipped TCPIP\$SERVICE.DAT ships ENABLED service line(s) -- the aux server would bind them (posture call). Comment them out (operator enables by default deliberately):"
    printf '%s\n' "$ENABLED" | sed 's/^/      /'
else
    ok "shipped TCPIP\$SERVICE.DAT ships with every service DISABLED (no enabled line)"
fi

# (c) SSH-specific posture (rd vms-843a): SSH runs as a DETACHED DAEMON started by
# @SYS$STARTUP:TCPIP$SSH_STARTUP (not an inetd service), so the shipped SSH-off
# posture is: the shipped SYSTARTUP must NOT invoke TCPIP$SSH_STARTUP. SSH-enable-
# by-default is a distinct, higher-stakes operator decision than daytime (a network
# login authority + a shipped host key), so assert it EXPLICITLY. A DCL invocation
# is a non-comment line ('$' col 1, not '$!') naming TCPIP$SSH_STARTUP.
if grep -nE '^\$[^!].*TCPIP\$SSH_STARTUP' "$SYSTARTUP" >/dev/null 2>&1; then
    bad "shipped SYSTARTUP_VMS.COM INVOKES @SYS\$STARTUP:TCPIP\$SSH_STARTUP -- that auto-starts the SSH daemon at boot (network login authority + shipped host key, Baron-reserved). The SSH cold-boot proof enables SSH via the OVMX_TEST_ENABLE_SSH test overlay only; if enabling SSH by default is intended, make it explicit and update this guard."
    grep -nE '^\$[^!].*TCPIP\$SSH_STARTUP' "$SYSTARTUP" | sed 's/^/      /'
else
    ok "shipped SYSTARTUP_VMS.COM does NOT invoke TCPIP\$SSH_STARTUP (SSH daemon not auto-started -- SSH is a layered product, off by default, Baron-reserved)"
fi

echo "=== test_tcpip_posture_guard: $([ "$FAIL" = 0 ] && echo PASS || echo FAIL) ==="
exit "$FAIL"
