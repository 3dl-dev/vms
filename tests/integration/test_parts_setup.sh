#!/bin/sh
#
# test_parts_setup.sh - BEHAVIOURAL gate (rd vms-977): PARTS_SETUP.COM does
# the pre-PCSI layered-product install: COPY the PARTS.EXE image out of the
# install kit (SYS$UPDATE:) into SYS$SYSTEM:, and DEFINE PARTS as a foreign
# command (PARTS :== $SYS$SYSTEM:PARTS.EXE) so it becomes a DCL verb.
#
# This exercises the COMMITTED procedure at
# distro/rootfs/vms/SYS0/SYSCOMMON/SYSUPD/PARTS_SETUP.COM directly -- not a
# copy of its text -- against a real vmsdcl (DCL.EXE), in an isolated
# VMS_ROOT so it never touches a shared /vms tree.
#
# HOST-MODE, NOT QEMU. COPY/DEFINE/global-symbol assignment are plain
# DCL+RMS facilities, already proven against a real /dev/vms executive
# elsewhere (vms-e5c/#177, vms-221/#179) -- this gate is about
# PARTS_SETUP.COM's OWN logic and placement, which needs no kernel executive
# to exercise (CLAUDE.md Rule 9's "executive facility" test requirement does
# not apply to a COPY/DEFINE/:== command procedure). The full boot ->
# @SYS$UPDATE:PARTS_SETUP.COM -> RUN PARTS chain under QEMU, with PARTS.EXE
# actually mastered into the image, is vms-cde's e2e gate.
#
# PARTS.EXE STAND-IN: this gate does not build the VMS-native PARTS.EXE
# (cc -> LINK.EXE -> IMGACT; that needs the musl link-native container, see
# tests/qemu/test_parts_rms_qemu.sh). It stages the host-built PARTS
# functional-test binary (src/apps/parts/, add_executable(PARTS ...)) under
# that name instead -- COPY and DEFINE do not care about a file's ELF
# contents, only that it exists and lands where the .COM says it does.
#
# Usage: test_parts_setup.sh [PATH-TO-DCL.EXE] [PATH-TO-PARTS-STANDIN] [REPO-ROOT]

set -u

DCL="${1:-${VMSDCL:-}}"
SRC_ROOT="${3:-$(cd "$(dirname "$0")/../.." && pwd)}"

if [ -z "$DCL" ]; then
    for cand in "$SRC_ROOT/build/bin/DCL.EXE" \
                "$SRC_ROOT/build/bin/vmsdcl"; do
        [ -x "$cand" ] && DCL="$cand" && break
    done
fi

PARTS_STANDIN="${2:-}"
if [ -z "$PARTS_STANDIN" ]; then
    for cand in "$SRC_ROOT/build/bin/PARTS" \
                "$SRC_ROOT/build/src/apps/parts/PARTS"; do
        [ -x "$cand" ] && PARTS_STANDIN="$cand" && break
    done
fi

status=0
passed=0
failed=0

if [ -z "$DCL" ] || [ ! -x "$DCL" ]; then
    echo "FAIL: no DCL.EXE to exercise (looked at argv[1], \$VMSDCL, build/bin)"
    echo "  -> this gate is BEHAVIOURAL; with no binary it is reported as"
    echo "     FAILED, never skipped. A skipped test is a failing test."
    exit 1
fi

if [ -z "$PARTS_STANDIN" ] || [ ! -f "$PARTS_STANDIN" ]; then
    echo "FAIL: no PARTS stand-in image to install (looked at argv[2], build/bin/PARTS)"
    echo "  -> build with -DBUILD_TESTS=ON so the PARTS target exists."
    exit 1
fi

SETUP_COM="$SRC_ROOT/distro/rootfs/vms/SYS0/SYSCOMMON/SYSUPD/PARTS_SETUP.COM"
if [ ! -f "$SETUP_COM" ]; then
    echo "FAIL: PARTS_SETUP.COM not found at $SETUP_COM"
    exit 1
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM

# Isolated VMS system disk -- DCL.EXE's setup_session() reads VMS_ROOT
# (dcl_main.c) instead of the real /vms, so this gate never touches shared
# host state.
VROOT="$WORK/vms_root"
mkdir -p "$VROOT/SYS0/SYSCOMMON/SYSEXE" \
         "$VROOT/SYS0/SYSCOMMON/SYSMGR" \
         "$VROOT/SYS0/SYSCOMMON/SYSLIB" \
         "$VROOT/SYS0/SYSCOMMON/SYSHLP" \
         "$VROOT/SYS0/SYSCOMMON/SYSUPD" \
         "$VROOT/SYSTMP" "$VROOT/USERS"

# Stage the REAL committed procedure plus a PARTS.EXE stand-in in the kit
# directory, exactly as a manager would find them after the kit was staged.
cp "$SETUP_COM" "$VROOT/SYS0/SYSCOMMON/SYSUPD/PARTS_SETUP.COM"
cp "$PARTS_STANDIN" "$VROOT/SYS0/SYSCOMMON/SYSUPD/PARTS.EXE"

echo "vms-977: PARTS_SETUP.COM installs PARTS (copy to SYS\$SYSTEM: + DEFINE foreign command)"
echo "  DCL under test:   $DCL"
echo "  PARTS_SETUP.COM:  $SETUP_COM"
echo "  PARTS stand-in:   $PARTS_STANDIN"

# DEFINE/SYSTEM SYS$UPDATE simulates what SYS$MANAGER:STARTUP.COM already
# does at real boot (distro/rootfs/.../SYSMGR/STARTUP.COM carries the same
# line) -- this gate is about PARTS_SETUP.COM, not about re-proving STARTUP.
VMS_ROOT="$VROOT" "$DCL" >"$WORK/out" 2>"$WORK/err" <<'DCLCMDS'
DEFINE/SYSTEM SYS$UPDATE SYS$SYSDEVICE:[SYS0.SYSCOMMON.SYSUPD]
@SYS$UPDATE:PARTS_SETUP.COM
SHOW SYMBOL PARTS
EXIT
DCLCMDS

# check <name> <extended-regex over stdout> <why-this-exists>
check() {
    if grep -qE "$2" "$WORK/out"; then
        echo "  PASS: $1"
        passed=$((passed + 1))
    else
        echo "  FAIL: $1"
        echo "        catches: $3"
        echo "        no line matched: $2"
        echo "        DCL stdout was:"
        sed 's/^/          | /' "$WORK/out"
        echo "        DCL stderr was:"
        sed 's/^/          | /' "$WORK/err"
        failed=$((failed + 1))
        status=1
    fi
}

check "prints the %PARTS-I-SETUP install-starting message" \
      '%PARTS-I-SETUP, OVMX PARTS demo installation starting' \
      "the procedure silently doing nothing, or failing before its first line"

check "prints the %PARTS-I-COPY step message" \
      '%PARTS-I-COPY, copying SYS\$UPDATE:PARTS\.EXE to SYS\$SYSTEM:' \
      "the COPY step being skipped or reordered"

check "prints the %PARTS-I-DEFINE step message" \
      '%PARTS-I-DEFINE, defining PARTS :== \$SYS\$SYSTEM:PARTS\.EXE' \
      "the DEFINE step being skipped or reordered"

check "prints the %PARTS-I-DONE completion message" \
      '%PARTS-I-DONE, PARTS demo installed - type PARTS to run it' \
      "the procedure aborting partway (SET NOON papering over a real failure)"

check "SHOW SYMBOL PARTS reports the foreign-command definition" \
      'PARTS = "\$SYS\$SYSTEM:PARTS\.EXE"' \
      "the :== assignment not landing as a GLOBAL symbol, or landing with the wrong value"

# Ground-source: the file actually landed under SYS$SYSTEM: (vmsfs stores
# VMS filenames lowercase on the Linux side -- src/vmsfs/vmsfs_translate.c
# str_downcase -- so this is deliberately case-insensitive).
if find "$VROOT/SYS0/SYSCOMMON/SYSEXE" -iname 'PARTS.EXE' -type f | grep -q .; then
    echo "  PASS: PARTS.EXE physically present under SYS\$SYSTEM: after COPY"
    passed=$((passed + 1))
else
    echo "  FAIL: PARTS.EXE physically present under SYS\$SYSTEM: after COPY"
    echo "        catches: COPY reporting success (or the messages above) without"
    echo "        the file actually landing -- a vacuous pass"
    ls -la "$VROOT/SYS0/SYSCOMMON/SYSEXE" | sed 's/^/          | /'
    failed=$((failed + 1))
    status=1
fi

echo ""
echo "vms-977 PARTS_SETUP.COM install gate: $passed passed, $failed failed"
exit $status
