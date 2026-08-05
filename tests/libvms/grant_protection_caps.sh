#!/bin/sh
#
# grant_protection_caps.sh - give test_libvms_protection the two capabilities
# its synthetic-UIC mechanism needs, on whatever host is about to run it
# (rd vms-bd7).
#
# WHY THIS EXISTS. test_protection.c cannot be handed a synthetic caller UIC as
# an argument, so it BECOMES one in a forked child, by whichever of two
# mechanisms the host offers (see run_chkpro_as):
#
#   synthesize_direct  setgid()+setuid(), needs CAP_SETGID + CAP_SETUID
#   synthesize_userns  CLONE_NEWUSER + uid_map/gid_map, needs unprivileged
#                      user namespaces
#
# Neither is available by default on the two hosts that matter, and they are
# unavailable for the SAME reason -- distro policy, not anything about OVMX:
#
#   GitHub Actions ubuntu-latest   AppArmor restricts unprivileged CLONE_NEWUSER
#   the workshop dev seat          kernel.apparmor_restrict_unprivileged_userns=1
#
# MEASURED on workshop, 2026-08-05, before this script existed: `unshare -Ur
# true` fails "write failed /proc/self/uid_map: Operation not permitted", and
# ctest test 37 test_libvms_protection failed 6 of its 9 assertions with
# "CLONE_NEWUSER/uid_map/gid_map setup failed on this host". After a single
# `sudo setcap cap_setgid,cap_setuid+ep` on the binary: 9/9, ALL TESTS PASSED.
# CI was already green only because .github/workflows/ci.yml carried that
# setcap as an inline step -- so the two hosts ran DIFFERENT harnesses and the
# dev seat got the red. That asymmetry is what this script removes: CI now
# calls this file, and so does a ctest fixture, so there is one mechanism.
#
# WHAT THIS IS NOT. It is not a skip and it must never become one. The grant is
# BEST EFFORT and this script always exits 0, because a host where the grant is
# impossible must still RUN test_libvms_protection and let it fail honestly --
# that suite's own rule is that a skipped discriminating case is a failing one,
# and a ctest fixture that exits nonzero would mark the test "Not Run", which
# is a skip wearing a different word. Everything this script decides is printed,
# so a run that did not get the capabilities says so before the failure it
# causes.
#
# The grant is also not persistent: setcap's file capabilities are dropped every
# time the binary is relinked. Running as a FIXTURES_SETUP fixture re-applies
# them on every ctest invocation, which is the reason it is a fixture and not a
# post-build step.
#
# Usage: grant_protection_caps.sh <path-to-test-binary>

set -u

BIN="${1:-}"
CAPS="cap_setgid,cap_setuid+ep"

echo "grant_protection_caps: target ${BIN:-<none>}"

if [ -z "$BIN" ] || [ ! -f "$BIN" ]; then
    echo "grant_protection_caps: no such binary -- nothing to grant."
    echo "  -> test_libvms_protection will fall back to synthesize_userns and"
    echo "     will fail honestly if this host also restricts user namespaces."
    exit 0
fi

# Already granted? Say so and stop. This is the common case on a rebuild-free
# re-run, and it keeps the script silent about sudo when sudo is not needed.
if command -v getcap >/dev/null 2>&1; then
    have=$(getcap "$BIN" 2>/dev/null || true)
    case "$have" in
        *cap_setgid*cap_setuid*|*cap_setuid*cap_setgid*)
            echo "grant_protection_caps: already granted ($have) -- nothing to do."
            exit 0
            ;;
    esac
fi

if ! command -v setcap >/dev/null 2>&1; then
    echo "grant_protection_caps: setcap is not on PATH (libcap2-bin)."
    echo "  -> falling through to synthesize_userns; if this host restricts"
    echo "     unprivileged user namespaces, test_libvms_protection will fail"
    echo "     and that failure is real, not a harness artifact."
    exit 0
fi

# Root first, then passwordless sudo. A sudo that would PROMPT is deliberately
# not attempted: ctest runs unattended, and a fixture that blocks on a password
# prompt is worse than one that reports it could not grant.
if [ "$(id -u)" = "0" ]; then
    if setcap "$CAPS" "$BIN"; then
        echo "grant_protection_caps: granted $CAPS as root."
        exit 0
    fi
    echo "grant_protection_caps: setcap failed as root (filesystem may be"
    echo "  mounted nosuid, or lack xattr support)."
    exit 0
fi

if command -v sudo >/dev/null 2>&1 && sudo -n true >/dev/null 2>&1; then
    if sudo -n setcap "$CAPS" "$BIN"; then
        echo "grant_protection_caps: granted $CAPS via passwordless sudo."
        exit 0
    fi
    echo "grant_protection_caps: sudo setcap failed (filesystem may be mounted"
    echo "  nosuid, or lack xattr support)."
    exit 0
fi

echo "grant_protection_caps: cannot grant -- not root and no passwordless sudo."
echo "  -> test_libvms_protection will try synthesize_userns instead. On a host"
echo "     that restricts unprivileged CLONE_NEWUSER (Ubuntu 24.04 default,"
echo "     kernel.apparmor_restrict_unprivileged_userns=1) its six"
echo "     discriminating assertions WILL fail. Fix by running once, by hand:"
echo "         sudo setcap $CAPS $BIN"
exit 0
